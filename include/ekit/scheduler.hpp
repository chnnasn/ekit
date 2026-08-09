#pragma once
// ekit - scheduler.hpp
//
// Scheduler runs systems in dependency order. Systems declare Reads/Writes;
// the scheduler builds a DAG (a writer is ordered before every reader/writer of
// the same component; two readers of the same component run in parallel) and
// executes independent systems concurrently on a small thread pool.
//
//   ekit::Scheduler scheduler(4); // 4 worker threads (0 == hardware concurrency)
//   scheduler.AddSystem(MoveSystem{})
//            .AddSystem(RenderSystem{});
//   scheduler.Run(world);

#include "world.hpp"
#include "system.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ekit {

namespace detail {

// Minimal fixed-size thread pool. Tasks are independent; WaitAll blocks until
// every submitted task has finished. Exceptions thrown inside tasks are
// captured and re-thrown by TakeError() after WaitAll.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count) {
        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) {
                            return;
                        }
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    }
                    try {
                        task();
                    } catch (...) {
                        std::lock_guard lock(mutex_);
                        if (!first_error_) {
                            first_error_ = std::current_exception();
                        }
                    }
                    {
                        std::lock_guard lock(mutex_);
                        --pending_;
                        if (pending_ == 0) {
                            done_cv_.notify_all();
                        }
                    }
                }
            });
        }
    }

    ~ThreadPool() {
        Shutdown();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F>
    void Submit(F&& f) {
        {
            std::lock_guard lock(mutex_);
            ++pending_;
            tasks_.emplace_back(std::forward<F>(f));
        }
        cv_.notify_one();
    }

    // Blocks until every submitted task has completed.
    void WaitAll() {
        std::unique_lock lock(mutex_);
        done_cv_.wait(lock, [this] { return pending_ == 0; });
    }

    // Returns and clears the first captured exception, if any.
    std::exception_ptr TakeError() {
        std::lock_guard lock(mutex_);
        std::exception_ptr error = first_error_;
        first_error_ = nullptr;
        return error;
    }

private:
    void Shutdown() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable done_cv_;
    std::size_t pending_ = 0;
    std::exception_ptr first_error_;
    bool stop_ = false;
};

} // namespace detail

class Scheduler {
public:
    // thread_count == 0 uses std::thread::hardware_concurrency().
    explicit Scheduler(std::size_t thread_count = 0)
        : thread_count_(thread_count == 0 ? std::max<std::size_t>(1, std::thread::hardware_concurrency())
                                          : thread_count) {}

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Registers a system (a class with Execute(World&) and optional
    // Reads/Writes declarations). Copies the system into the scheduler.
    template<typename T>
    Scheduler& AddSystem(T&& system) {
        static_assert(detail::IsSystemLike<std::decay_t<T>>::value,
                      "ekit: Scheduler::AddSystem requires a system object with an Execute(World&) method.");
        systems_.push_back(std::make_unique<detail::SystemWrapper<std::decay_t<T>>>(
            std::forward<T>(system)));
        return *this;
    }

    Scheduler& SetThreadCount(std::size_t count) {
        thread_count_ = std::max<std::size_t>(1, count);
        return *this;
    }

    std::size_t GetThreadCount() const {
        return thread_count_;
    }

    std::size_t GetSystemCount() const {
        return systems_.size();
    }

    void Clear() {
        systems_.clear();
    }

    // Builds the dependency graph and executes all systems. Independent
    // systems run in parallel on the internal thread pool.
    void Run(World& world) {
        RunImpl(world, true);
    }

    // Same as Run but executes everything sequentially in dependency order.
    void RunSingleThreaded(World& world) {
        RunImpl(world, false);
    }

private:
    struct SystemDeps {
        std::vector<ComponentTypeId> reads;
        std::vector<ComponentTypeId> writes;
    };

    static bool Overlaps(const std::vector<ComponentTypeId>& a,
                         const std::vector<ComponentTypeId>& b) {
        for (ComponentTypeId x : a) {
            for (ComponentTypeId y : b) {
                if (x == y) {
                    return true;
                }
            }
        }
        return false;
    }

    void RunImpl(World& world, bool parallel) {
        const std::size_t n = systems_.size();
        if (n == 0) {
            return;
        }

        // Resolve declared dependencies to runtime component ids.
        std::vector<SystemDeps> deps(n);
        for (std::size_t i = 0; i < n; ++i) {
            systems_[i]->GetDependencies(world, deps[i].reads, deps[i].writes);
        }

        // depends[i][j] == true  =>  system i must finish before system j.
        // A writer is ordered before every reader/writer of the same component;
        // two writers of the same component form a cycle (genuine conflict).
        std::vector<std::vector<bool>> depends(n, std::vector<bool>(n, false));
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                if (i == j) {
                    continue;
                }
                if (Overlaps(deps[i].writes, deps[j].reads) ||
                    Overlaps(deps[i].writes, deps[j].writes)) {
                    depends[i][j] = true; // i writes something j reads/writes
                }
                if (Overlaps(deps[j].writes, deps[i].reads) ||
                    Overlaps(deps[j].writes, deps[i].writes)) {
                    depends[j][i] = true; // j writes something i reads/writes
                }
            }
        }

        // Kahn's algorithm: topological levels (longest-path).
        std::vector<std::size_t> indegree(n, 0);
        std::vector<std::vector<std::size_t>> dependents(n);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                if (depends[i][j]) {
                    ++indegree[j];
                    dependents[i].push_back(j);
                }
            }
        }

        std::queue<std::size_t> ready;
        for (std::size_t i = 0; i < n; ++i) {
            if (indegree[i] == 0) {
                ready.push(i);
            }
        }

        std::vector<std::size_t> level(n, 0);
        std::size_t processed = 0;
        while (!ready.empty()) {
            const std::size_t i = ready.front();
            ready.pop();
            ++processed;
            for (std::size_t j : dependents[i]) {
                level[j] = std::max(level[j], level[i] + 1);
                if (--indegree[j] == 0) {
                    ready.push(j);
                }
            }
        }

        if (processed != n) {
            throw EkitException(
                "ekit: Scheduler detected a dependency cycle among systems. "
                "Review the Reads/Writes declarations of your systems.");
        }

        std::size_t max_level = 0;
        for (std::size_t l : level) {
            max_level = std::max(max_level, l);
        }
        std::vector<std::vector<std::size_t>> buckets(max_level + 1);
        for (std::size_t i = 0; i < n; ++i) {
            buckets[level[i]].push_back(i);
        }

        // Execute level by level; systems in the same level are independent.
        for (const auto& bucket : buckets) {
            if (!parallel || bucket.size() <= 1 || thread_count_ <= 1) {
                for (std::size_t i : bucket) {
                    systems_[i]->Execute(world);
                }
            } else {
                EnsurePool();
                for (std::size_t i : bucket) {
                    pool_->Submit([this, &world, i] { systems_[i]->Execute(world); });
                }
                pool_->WaitAll();
                if (std::exception_ptr error = pool_->TakeError()) {
                    std::rethrow_exception(error);
                }
            }
        }
    }

    void EnsurePool() {
        if (!pool_) {
            pool_ = std::make_unique<detail::ThreadPool>(thread_count_);
        }
    }

    std::vector<std::unique_ptr<ISystem>> systems_;
    std::unique_ptr<detail::ThreadPool> pool_;
    std::size_t thread_count_;
};

} // namespace ekit

