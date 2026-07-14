#include <mcraw/core/timing.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

#include <nlohmann/json.hpp>

namespace mcraw {
namespace {

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const double index = p * static_cast<double>(sorted.size() - 1U);
    const auto lo = static_cast<std::size_t>(std::floor(index));
    const auto hi = static_cast<std::size_t>(std::ceil(index));
    const double fraction = index - static_cast<double>(lo);
    return sorted[lo] * (1.0 - fraction) + sorted[hi] * fraction;
}

} // namespace

void StageTimings::add(std::string stage, double milliseconds) {
    values_[std::move(stage)].push_back(milliseconds);
}

void StageTimings::merge(const StageTimings& other) {
    for (const auto& [stage, samples] : other.values_) {
        auto& destination = values_[stage];
        destination.insert(destination.end(), samples.begin(), samples.end());
    }
}

TimingSummary StageTimings::summary(const std::string& stage) const {
    const auto found = values_.find(stage);
    if (found == values_.end() || found->second.empty()) return {};
    auto sorted = found->second;
    std::sort(sorted.begin(), sorted.end());
    const double total = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    return {
        sorted.size(), total, total / static_cast<double>(sorted.size()),
        percentile(sorted, 0.50), percentile(sorted, 0.95), percentile(sorted, 0.99)
    };
}

nlohmann::json StageTimings::to_json() const {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [name, ignored] : values_) {
        static_cast<void>(ignored);
        const auto item = summary(name);
        result[name] = {
            {"samples", item.samples}, {"total_ms", item.total_ms},
            {"mean_ms", item.mean_ms}, {"p50_ms", item.p50_ms},
            {"p95_ms", item.p95_ms}, {"p99_ms", item.p99_ms}
        };
    }
    return result;
}

StageTimer::StageTimer(StageTimings& timings, std::string stage)
    : timings_(timings), stage_(std::move(stage)), start_(std::chrono::steady_clock::now()) {}

StageTimer::~StageTimer() {
    const auto elapsed = std::chrono::steady_clock::now() - start_;
    timings_.add(stage_, std::chrono::duration<double, std::milli>(elapsed).count());
}

} // namespace mcraw
