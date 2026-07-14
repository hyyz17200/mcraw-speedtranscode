#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace mcraw {

struct TimingSummary {
    std::size_t samples{};
    double total_ms{};
    double mean_ms{};
    double p50_ms{};
    double p95_ms{};
    double p99_ms{};
};

class StageTimings {
public:
    void add(std::string stage, double milliseconds);
    void merge(const StageTimings& other);
    [[nodiscard]] TimingSummary summary(const std::string& stage) const;
    [[nodiscard]] nlohmann::json to_json() const;

private:
    std::map<std::string, std::vector<double>, std::less<>> values_;
};

class StageTimer {
public:
    StageTimer(StageTimings& timings, std::string stage);
    ~StageTimer();

    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;

private:
    StageTimings& timings_;
    std::string stage_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace mcraw
