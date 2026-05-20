#include "L3KVG/ThreadPool.hpp"

// IMPLEMENTATION FIREWALL: Citor and ConcurrentQueue are isolated here
#include <citor.hpp>
#include <concurrentqueue.h>
#include <iostream>

namespace l3kvg {

struct ThreadPool::Impl {
    citor::ThreadPool pool;
    moodycamel::ConcurrentQueue<std::function<void()>> queue;
    std::thread driver_thread;
    std::atomic<bool> stop{false};

    explicit Impl(size_t threads) : pool(threads + 1) {
        driver_thread = std::thread([this] {
            std::function<void()> task;
            while (!stop.load(std::memory_order_relaxed)) {
                if (queue.try_dequeue(task)) {
                    // Dispatch to citor using its customization point object.
                    // Sub-microsecond handoff to the work-stealing compute engine.
                    pool.template parallelFor<citor::HintsDefaults>(0, 1, [&](size_t, size_t) {
                        task();
                    });
                } else {
                    // Low-power yield when idle
                    std::this_thread::yield();
                }
            }
        });
    }

    ~Impl() {
        stop = true;
        if (driver_thread.joinable()) driver_thread.join();
    }
};

ThreadPool::ThreadPool(size_t threads) : pimpl_(std::make_unique<Impl>(threads)) {}

ThreadPool::~ThreadPool() = default;

void ThreadPool::push_task_internal(std::function<void()> task) {
    if (pimpl_->stop.load(std::memory_order_relaxed)) {
        throw std::runtime_error("push_task on stopped ThreadPool");
    }
    pimpl_->queue.enqueue(std::move(task));
}

void ThreadPool::parallel_for(size_t first, size_t last, std::function<void(size_t, size_t)> fn) {
    // Direct synchronous parallel execution via Citor
    pimpl_->pool.template parallelFor<citor::HintsDefaults>(first, last, std::move(fn));
}

} // namespace l3kvg
