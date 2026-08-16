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
#include "parallel.hpp"

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
        thread_count_ = count == 0 ? std::max<std::size_t>(1, std::thread::hardware_concurrency())
                                   : count;
        // Rebuild the pool lazily on the next parallel Run; an existing pool
        // cannot change its worker count in place.
        pool_.reset();
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

    // Kahn's algorithm: true when the dependency graph has no cycle.
    static bool IsAcyclic(const std::vector<std::vector<bool>>& depends) {
        const std::size_t n = depends.size();
        std::vector<std::size_t> indegree(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                if (depends[i][j]) {
                    ++indegree[j];
                }
            }
        }
        std::queue<std::size_t> ready;
        for (std::size_t i = 0; i < n; ++i) {
            if (indegree[i] == 0) {
                ready.push(i);
            }
        }
        std::size_t processed = 0;
        while (!ready.empty()) {
            const std::size_t i = ready.front();
            ready.pop();
            ++processed;
            for (std::size_t j = 0; j < n; ++j) {
                if (depends[i][j] && --indegree[j] == 0) {
                    ready.push(j);
                }
            }
        }
        return processed == n;
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
        //
        // Hard edges: a writer is ordered before every reader of the same
        // component (writes_i ? reads_j -> i before j). If these alone form a
        // cycle, the declared dependencies genuinely contradict each other and
        // we report a cycle.
        std::vector<std::vector<bool>> depends(n, std::vector<bool>(n, false));
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                if (i == j) {
                    continue;
                }
                if (Overlaps(deps[i].writes, deps[j].reads)) {
                    depends[i][j] = true; // i writes something j reads
                }
            }
        }

        if (!IsAcyclic(depends)) {
            throw EkitException(
                "ekit: Scheduler detected a genuine dependency conflict: the "
                "Reads/Writes declarations form a cycle. Review the declarations "
                "of your systems.");
        }

        // Soft edges: two writers of the same component do NOT form a cycle -
        // they are serialized in registration order (the earlier-registered
        // system runs first). Such an edge is only a tie-breaker, so it is
        // dropped when it would conflict with the hard dataflow edges.
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                if (!Overlaps(deps[i].writes, deps[j].writes)) {
                    continue;
                }
                depends[i][j] = true; // i was registered before j
                if (!IsAcyclic(depends)) {
                    depends[i][j] = false;
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
                "ekit: Scheduler detected a genuine dependency conflict: the "
                "Reads/Writes declarations form a cycle. Review the declarations "
                "of your systems.");
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
            pool_ = std::make_unique<ThreadPool>(thread_count_);
        }
    }

    std::vector<std::unique_ptr<ISystem>> systems_;
    std::unique_ptr<ThreadPool> pool_;
    std::size_t thread_count_;
};

} // namespace ekit

