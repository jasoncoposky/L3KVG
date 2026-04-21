#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

namespace l3kvg {

class ThreadPool {
public:
    explicit ThreadPool(size_t threads) : stop_(false) {
        for(size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] {
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex_);
                        this->condition_.wait(lock, [this]{ return this->stop_ || !this->tasks_.empty(); });
                        if(this->stop_ && this->tasks_.empty()) return;
                        task = std::move(this->tasks_.front());
                        this->tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;
        
        // Use shared_ptr to wrap move-only types to satisfy potential copy requirements in some compiler implementations
        auto task_fn = std::make_shared<std::decay_t<F>>(std::forward<F>(f));
        
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [task_fn, ...args = std::forward<Args>(args)]() mutable {
                return std::invoke(std::move(*task_fn), std::move(args)...);
            }
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if(stop_) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks_.emplace([task](){ (*task)(); });
        }
        condition_.notify_one();
        return res;
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for(std::thread &worker: workers_)
            worker.join();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_;
};

} // namespace l3kvg
