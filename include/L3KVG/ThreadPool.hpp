#pragma once

#include <vector>
#include <thread>
#include <functional>
#include <future>
#include <atomic>
#include <memory>
#include <stdexcept>

// Citor: High-Performance, Topology-Aware Compute Engine
#include <citor.hpp>

// Moodycamel: Lock-Free Concurrent Queue for minimal handoff latency
#include <concurrentqueue.h>

namespace l3kvg {

class ThreadPool {
public:
    explicit ThreadPool(size_t threads) 
        : pool_(threads + 1), // +1 for the local producer/driver
          stop_(false) {
        
        // Driver Thread: Drains the async queue and dispatches to citor workers
        driver_thread_ = std::thread([this] {
            std::function<void()> task;
            while (!stop_.load(std::memory_order_relaxed)) {
                if (queue_.try_dequeue(task)) {
                    // Dispatch to citor. Using parallelFor with 1 item 
                    // allows citor to handle the worker handoff with sub-microsecond latency.
                    pool_.template parallelFor<citor::HintsDefaults>(0, 1, [&](size_t, size_t) {
                        task();
                    });
                } else {
                    // Adaptive backoff for idle driver
                    std::this_thread::yield();
                }
            }
        });
    }

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;
        
        auto task_fn = std::make_shared<std::decay_t<F>>(std::forward<F>(f));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [task_fn, ...args = std::forward<Args>(args)]() mutable {
                return std::invoke(std::move(*task_fn), std::move(args)...);
            }
        );
        
        std::future<return_type> res = task->get_future();
        
        if (stop_.load(std::memory_order_relaxed)) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }

        // Lock-free push: minimal latency for the producer thread (e.g. ZMQ loop)
        queue_.enqueue([task](){ (*task)(); });
        
        return res;
    }

    ~ThreadPool() {
        stop_ = true;
        if (driver_thread_.joinable()) driver_thread_.join();
    }

    // Access to the underlying citor pool for direct parallel operations
    citor::ThreadPool& get_citor_pool() { return pool_; }

private:
    citor::ThreadPool pool_;
    moodycamel::ConcurrentQueue<std::function<void()>> queue_;
    std::thread driver_thread_;
    std::atomic<bool> stop_;
};

} // namespace l3kvg
