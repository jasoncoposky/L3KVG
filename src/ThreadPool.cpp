#include "L3KVG/ThreadPool.hpp"
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <unistd.h>

namespace l3kvg {

struct ThreadPool::Impl {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    size_t num_threads{1};
    pid_t creator_pid{getpid()};

    explicit Impl(size_t threads) : num_threads(threads > 0 ? threads : 1) {
        workers.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        cv.wait(lock, [this] {
                            return stop.load(std::memory_order_relaxed) || !tasks.empty();
                        });
                        if (stop.load(std::memory_order_relaxed) && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
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
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            stop.store(true, std::memory_order_relaxed);
        }
        cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

    void push_task(std::function<void()> task) {
        if (stop.load(std::memory_order_relaxed)) {
            throw std::runtime_error("push_task on stopped ThreadPool");
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            tasks.push(std::move(task));
        }
        cv.notify_one();
    }

    void parallel_for(size_t first, size_t last, std::function<void(size_t, size_t)> fn) {
        if (first >= last) return;
        size_t total = last - first;
        if (num_threads <= 1 || total <= 1) {
            fn(first, last);
            return;
        }
        size_t chunks = std::min(num_threads, total);
        size_t chunk_size = total / chunks;
        size_t remainder = total % chunks;
        std::vector<std::future<void>> futures;
        futures.reserve(chunks - 1);
        size_t start = first;
        for (size_t i = 0; i < chunks; ++i) {
            size_t count = chunk_size + (i < remainder ? 1 : 0);
            size_t end = start + count;
            if (i == chunks - 1) {
                fn(start, end);
            } else {
                std::packaged_task<void()> pt([fn, start, end] { fn(start, end); });
                futures.push_back(pt.get_future());
                push_task([pt = std::make_shared<std::packaged_task<void()>>(std::move(pt))]() {
                    (*pt)();
                });
            }
            start = end;
        }
        for (auto& fut : futures) fut.get();
    }
};

ThreadPool::ThreadPool(size_t threads) : pimpl_(std::make_unique<Impl>(threads)) {}
ThreadPool::~ThreadPool() = default;
void ThreadPool::push_task_internal(std::function<void()> task) { pimpl_->push_task(std::move(task)); }
void ThreadPool::parallel_for(size_t first, size_t last, std::function<void(size_t, size_t)> fn) { pimpl_->parallel_for(first, last, std::move(fn)); }

} // namespace l3kvg
