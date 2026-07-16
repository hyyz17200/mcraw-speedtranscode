#include <mcraw/core/worker_pool.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

TEST_CASE("persistent worker pool bounds work and preserves ordered future delivery") {
    mcraw::PersistentWorkerPool pool(2, 2);
    std::mutex thread_mutex;
    std::set<std::thread::id> worker_threads;
    std::vector<std::future<std::size_t>> futures;
    for (std::size_t i = 0; i < 12; ++i) {
        futures.push_back(pool.submit([i, &thread_mutex, &worker_threads] {
            {
                std::scoped_lock lock(thread_mutex);
                worker_threads.insert(std::this_thread::get_id());
            }
            if ((i % 2U) == 0U) std::this_thread::sleep_for(1ms);
            return i;
        }));
    }
    for (std::size_t i = 0; i < futures.size(); ++i) REQUIRE(futures[i].get() == i);

    const auto telemetry = pool.telemetry();
    REQUIRE(worker_threads.size() <= 2U);
    REQUIRE(telemetry.workers == 2U);
    REQUIRE(telemetry.queue_capacity == 2U);
    REQUIRE(telemetry.max_queue_depth <= telemetry.queue_capacity);
    REQUIRE(telemetry.tasks_started == 12U);
    REQUIRE(telemetry.tasks_completed == 12U);
    REQUIRE(telemetry.tasks_cancelled == 0U);
}

TEST_CASE("persistent worker pool propagates task errors and cancels queued work") {
    SECTION("task exception reaches the ordered consumer") {
        mcraw::PersistentWorkerPool pool(1, 1);
        auto failed = pool.submit([]() -> int { throw std::runtime_error("decoder failed"); });
        REQUIRE_THROWS_AS(failed.get(), std::runtime_error);
    }

    SECTION("explicit cancellation drops work that has not started") {
        mcraw::PersistentWorkerPool pool(1, 3);
        std::promise<void> started;
        auto started_future = started.get_future();
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto active = pool.submit([gate, &started] {
            started.set_value();
            gate.wait();
        });
        started_future.wait();
        auto queued_a = pool.submit([] { return 41; });
        auto queued_b = pool.submit([] { return 42; });
        pool.cancel();
        release.set_value();
        active.get();
        REQUIRE_THROWS_AS(queued_a.get(), std::future_error);
        REQUIRE_THROWS_AS(queued_b.get(), std::future_error);
        REQUIRE(pool.telemetry().tasks_cancelled == 2U);
    }
}
