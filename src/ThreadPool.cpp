#include "L3KVG/ThreadPool.hpp"
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <unistd.h>

namespace l3kvg {

namespace {
thread_local bool tl_in_pool_worker = false;
thread_local size_t tl_worker_index = 0;
} // namespace

struct ThreadPool::Impl {
    struct alignas(64) WorkerQueue {
        std::deque<std::function<void()>> tasks;
        std::mutex mu;
        std::condition_variable cv;
    };

    std::vector<WorkerQueue> queues;
    std::vector<std::thread> workers;
    std::atomic<bool> stop{false};
    std::atomic<size_t> next_queue{0};
    size_t num_threads{1};
    pid_t creator_pid{getpid()};

    explicit Impl(size_t threads)
        : queues(threads > 0 ? threads : 1),
          num_threads(threads > 0 ? threads : 1) {
        workers.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this, i] {
                tl_in_pool_worker = true;
                tl_worker_index = i;
                auto& my_q = queues[i];
                while (true) {
                    std::function<void()> task;
                    {
                        std::lock_guard<std::mutex> lock(my_q.mu);
                        if (!my_q.tasks.empty()) {
                            task = std::move(my_q.tasks.front());
                            my_q.tasks.pop_front();
                        }
                    }

                    if (!task && num_threads > 1) {
                        for (size_t offset = 1; offset < num_threads; ++offset) {
                            auto& victim = queues[(i + offset) % num_threads];
                            std::unique_lock<std::mutex> vlock(victim.mu, std::try_to_lock);
                            if (vlock.owns_lock() && !victim.tasks.empty()) {
                                task = std::move(victim.tasks.back());
                                victim.tasks.pop_back();
                                break;
                            }
                        }
                    }

                    if (!task) {
                        std::unique_lock<std::mutex> lock(my_q.mu);
                        my_q.cv.wait(lock, [this, &my_q] {
                            return stop.load(std::memory_order_acquire) || !my_q.tasks.empty();
                        });
                        if (stop.load(std::memory_order_acquire) && my_q.tasks.empty()) {
                            return;
                        }
                        if (!my_q.tasks.empty()) {
                            task = std::move(my_q.tasks.front());
                            my_q.tasks.pop_front();
                        }
                    }

                    if (task) {
                        try {
                            task();
                        } catch (...) {
                            // Suppress exceptions from tasks to prevent std::terminate
                        }
                    }
                }
            });
        }
    }

    ~Impl() {
        if (getpid() != creator_pid) {
            // In a forked child process, parent worker threads do not exist.
            // Joining them causes undefined behavior or deadlocks.
            for (auto& worker : workers) {
                if (worker.joinable()) worker.detach();
            }
            return;
        }
        stop.store(true, std::memory_order_release);
        for (auto& q : queues) {
            std::lock_guard<std::mutex> lock(q.mu);
            q.cv.notify_all();
        }
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

    void push_task(std::function<void()> task) {
        if (stop.load(std::memory_order_acquire)) {
            throw std::runtime_error("push_task on stopped ThreadPool");
        }
        size_t idx = tl_in_pool_worker
                         ? tl_worker_index
                         : (next_queue.fetch_add(1, std::memory_order_relaxed) % num_threads);
        auto& q = queues[idx];
        {
            std::lock_guard<std::mutex> lock(q.mu);
            q.tasks.push_back(std::move(task));
        }
        q.cv.notify_one();
    }

    void parallel_for(size_t first, size_t last, std::function<void(size_t, size_t)> fn) {
        if (first >= last) return;
        size_t total = last - first;
        if (tl_in_pool_worker || workers.empty() || num_threads <= 1 || total < 64) {
            fn(first, last);
            return;
        }
        size_t chunks = std::min(num_threads, total / 32);
        if (chunks <= 1) {
            fn(first, last);
            return;
        }
        size_t chunk_size = total / chunks;
        size_t remainder = total % chunks;

        std::atomic<size_t> remaining{chunks - 1};
        std::mutex done_mu;
        std::condition_variable done_cv;
        std::exception_ptr first_exc;
        std::mutex exc_mu;

        size_t start = first;
        for (size_t i = 0; i < chunks; ++i) {
            size_t count = chunk_size + (i < remainder ? 1 : 0);
            size_t end = start + count;
            if (i == chunks - 1) {
                try {
                    fn(start, end);
                } catch (...) {
                    std::lock_guard<std::mutex> lk(exc_mu);
                    if (!first_exc) first_exc = std::current_exception();
                }
            } else {
                auto& q = queues[i % num_threads];
                {
                    std::lock_guard<std::mutex> lock(q.mu);
                    q.tasks.push_back([&fn, start, end, &remaining, &done_mu, &done_cv, &first_exc, &exc_mu] {
                        try {
                            fn(start, end);
                        } catch (...) {
                            std::lock_guard<std::mutex> lk(exc_mu);
                            if (!first_exc) first_exc = std::current_exception();
                        }
                        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            std::lock_guard<std::mutex> lk(done_mu);
                            done_cv.notify_one();
                        }
                    });
                }
                q.cv.notify_one();
            }
            start = end;
        }

        if (remaining.load(std::memory_order_acquire) > 0) {
            std::unique_lock<std::mutex> lk(done_mu);
            done_cv.wait(lk, [&remaining] {
                return remaining.load(std::memory_order_acquire) == 0;
            });
        }
        if (first_exc) {
            std::rethrow_exception(first_exc);
        }
    }
};

ThreadPool::ThreadPool(size_t threads) : pimpl_(std::make_unique<Impl>(threads)) {}
ThreadPool::~ThreadPool() = default;
void ThreadPool::push_task_internal(std::function<void()> task) { pimpl_->push_task(std::move(task)); }
void ThreadPool::parallel_for(size_t first, size_t last, std::function<void(size_t, size_t)> fn) { pimpl_->parallel_for(first, last, std::move(fn)); }
size_t ThreadPool::size() const noexcept { return pimpl_->num_threads; }
bool ThreadPool::is_worker_thread() noexcept { return tl_in_pool_worker; }

} // namespace l3kvg
