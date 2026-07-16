#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <deque>

namespace mcraw {

struct WorkerPoolTelemetry {
    std::size_t workers{};
    std::size_t queue_capacity{};
    std::size_t max_queue_depth{};
    std::uint64_t submit_waits{};
    double submit_wait_ms{};
    std::uint64_t tasks_started{};
    std::uint64_t tasks_completed{};
    std::uint64_t tasks_cancelled{};
};

class PersistentWorkerPool {
public:
    explicit PersistentWorkerPool(std::size_t workers,
                                  std::size_t queue_capacity = 0U)
        : queue_capacity_(queue_capacity == 0U ? workers : queue_capacity) {
        if (workers == 0U || queue_capacity_ == 0U) {
            throw std::invalid_argument("worker count and queue capacity must be positive");
        }
        telemetry_.workers = workers;
        telemetry_.queue_capacity = queue_capacity_;
        workers_.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            workers_.emplace_back([this](std::stop_token stop) { run(stop); });
        }
    }

    ~PersistentWorkerPool() { cancel(); }

    PersistentWorkerPool(const PersistentWorkerPool&) = delete;
    PersistentWorkerPool& operator=(const PersistentWorkerPool&) = delete;

    template <typename Function>
    auto submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto future = task->get_future();

        std::unique_lock lock(mutex_);
        const bool must_wait = tasks_.size() >= queue_capacity_;
        const auto wait_start = std::chrono::steady_clock::now();
        space_available_.wait(lock, [this] {
            return !accepting_ || tasks_.size() < queue_capacity_;
        });
        if (must_wait) {
            ++telemetry_.submit_waits;
            telemetry_.submit_wait_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wait_start).count();
        }
        if (!accepting_) throw std::runtime_error("worker pool is not accepting work");
        tasks_.emplace_back([task] { (*task)(); });
        telemetry_.max_queue_depth = std::max(telemetry_.max_queue_depth, tasks_.size());
        work_available_.notify_one();
        return future;
    }

    void cancel() noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (!accepting_) return;
            accepting_ = false;
            telemetry_.tasks_cancelled += tasks_.size();
            tasks_.clear();
        }
        for (auto& worker : workers_) worker.request_stop();
        work_available_.notify_all();
        space_available_.notify_all();
    }

    [[nodiscard]] WorkerPoolTelemetry telemetry() const {
        std::scoped_lock lock(mutex_);
        return telemetry_;
    }

private:
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                work_available_.wait(lock, stop, [this] {
                    return !accepting_ || !tasks_.empty();
                });
                if (stop.stop_requested()) return;
                if (tasks_.empty()) {
                    if (!accepting_) return;
                    continue;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
                ++telemetry_.tasks_started;
                space_available_.notify_one();
            }
            task();
            {
                std::scoped_lock lock(mutex_);
                ++telemetry_.tasks_completed;
            }
        }
    }

    const std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable_any work_available_;
    std::condition_variable space_available_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::jthread> workers_;
    bool accepting_{true};
    WorkerPoolTelemetry telemetry_;
};

} // namespace mcraw
