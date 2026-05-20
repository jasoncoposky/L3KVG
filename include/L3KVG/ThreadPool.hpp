#pragma once

#include <vector>
#include <thread>
#include <functional>
#include <future>
#include <atomic>
#include <memory>
#include <stdexcept>

namespace l3kvg {

/**
 * ThreadPool: High-performance, topology-aware compute engine.
 * 
 * IMPLEMENTATION FIREWALL: This class uses the PIMPL pattern to hide the complex
 * citor.hpp template library from the rest of the codebase. This ensures fast 
 * compilation while retaining microsecond-scale task dispatch and L3 cache locality.
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * enqueue: Asynchronously offload a task to the compute pool.
     * Uses a lock-free concurrent queue for minimal handoff latency.
     */
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
        
        push_task_internal([task](){ (*task)(); });
        
        return res;
    }

    /**
     * parallel_for: Synchronously execute a range in parallel across the pool.
     * Ideal for graph traversals and batch operations.
     */
    void parallel_for(size_t first, size_t last, std::function<void(size_t, size_t)> fn);

private:
    void push_task_internal(std::function<void()> task);

    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace l3kvg
