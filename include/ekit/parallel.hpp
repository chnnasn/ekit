#pragma once
// ekit - parallel.hpp
//
// A minimal, reusable fixed-size thread pool plus a chunked parallel-for
// helper. Used by Query::ForEachParallel and by the Scheduler.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ekit {

// Fixed-size thread pool. Tasks are independent; WaitAll blocks until every
// submitted task has finished. Exceptions thrown inside tasks are captured and
// re-thrown by TakeError() after WaitAll.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count) {
        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~ThreadPool() {
        Shutdown();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    std::size_t ThreadCount() const {
        return workers_.size();
    }

    template<typename F>
    void Submit(F&& f) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++pending_;
            tasks_.emplace_back(std::forward<F>(f));
        }
        cv_.notify_one();
    }

    // Blocks until every submitted task has completed.
    void WaitAll() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this] { return pending_ == 0; });
    }

    // Returns and clears the first captured exception, if any.
    std::exception_ptr TakeError() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::exception_ptr error = first_error_;
        first_error_ = nullptr;
        return error;
    }

private:
    void WorkerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
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
                std::lock_guard<std::mutex> lock(mutex_);
                if (!first_error_) {
                    first_error_ = std::current_exception();
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --pending_;
                if (pending_ == 0) {
                    done_cv_.notify_all();
                }
            }
        }
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
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

namespace detail {

// Runs fn(begin, end) over index ranges covering [0, count).
//
// Work is grabbed dynamically through a single atomic cursor: each worker
// repeatedly claims the next chunk until none remain. This keeps the number of
// queue submissions fixed at one per worker (instead of one per chunk), so the
// scheduling overhead stays flat as the number of chunks grows.
template<typename Index, typename F>
void ParallelFor(ThreadPool& pool, Index count, F&& fn) {
    const std::size_t workers = pool.ThreadCount();
    if (workers <= 1 || count <= 1) {
        fn(static_cast<Index>(0), count);
        return;
    }

    const std::size_t n = static_cast<std::size_t>(count);
    constexpr std::size_t kMinChunk = 64;

    // Tiny workloads are faster run serially than paying the fork/join cost.
    if (n < workers * kMinChunk) {
        fn(static_cast<Index>(0), count);
        return;
    }

    const std::size_t chunk = std::max<std::size_t>(kMinChunk, n / (workers * 4));
    std::atomic<std::size_t> next{0};

    auto loop = [&next, n, chunk, &fn] {
        for (;;) {
            const std::size_t i = next.fetch_add(chunk, std::memory_order_relaxed);
            if (i >= n) {
                break;
            }
            fn(static_cast<Index>(i), static_cast<Index>(std::min<std::size_t>(n, i + chunk)));
        }
    };

    for (std::size_t i = 0; i < workers; ++i) {
        pool.Submit(loop);
    }
    pool.WaitAll();
    if (std::exception_ptr error = pool.TakeError()) {
        std::rethrow_exception(error);
    }
}

} // namespace detail
} // namespace ekit
