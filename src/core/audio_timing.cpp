#include <mcraw/core/audio_timing.hpp>

#include <algorithm>
#include <limits>

#include <nlohmann/json.hpp>

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;

[[noreturn]] void invalid_timing(const char* message) {
    throw Error(ErrorCode::invalid_container, message);
}

std::int64_t checked_add(std::int64_t left, std::int64_t right) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        invalid_timing("audio timeline exceeds the supported integer range");
    }
    return left + right;
}

std::int64_t checked_subtract(std::int64_t left, std::int64_t right) {
    if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
        (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right)) {
        invalid_timing("audio timestamp residual exceeds the supported integer range");
    }
    return left - right;
}

std::int64_t absolute_checked(std::int64_t value) {
    if (value == std::numeric_limits<std::int64_t>::min()) {
        invalid_timing("audio timestamp residual exceeds the supported integer range");
    }
    return value < 0 ? -value : value;
}

void require_rescale_arguments(std::int64_t value, int sample_rate) {
    if (value < 0) invalid_timing("audio timeline values must not be negative");
    if (sample_rate <= 0) invalid_timing("audio sample rate must be positive");
}

std::int64_t checked_quotient_product(std::int64_t quotient, std::int64_t scale) {
    if (quotient > std::numeric_limits<std::int64_t>::max() / scale) {
        invalid_timing("audio timeline rescale exceeds the supported integer range");
    }
    return quotient * scale;
}

bool exceeds_twice(std::int64_t value, std::int64_t threshold) {
    return value > threshold && value - threshold > threshold;
}

} // namespace

std::int64_t samples_to_ns_nearest(std::int64_t samples, int sample_rate) {
    require_rescale_arguments(samples, sample_rate);
    const auto rate = static_cast<std::int64_t>(sample_rate);
    const auto whole = checked_quotient_product(samples / rate, nanoseconds_per_second);
    const auto remainder = samples % rate;
    const auto fraction = (remainder * nanoseconds_per_second + rate / 2) / rate;
    return checked_add(whole, fraction);
}

std::int64_t ns_to_samples_nearest(std::int64_t nanoseconds, int sample_rate) {
    require_rescale_arguments(nanoseconds, sample_rate);
    const auto rate = static_cast<std::int64_t>(sample_rate);
    const auto whole = checked_quotient_product(nanoseconds / nanoseconds_per_second, rate);
    const auto remainder = nanoseconds % nanoseconds_per_second;
    const auto fraction = (remainder * rate + nanoseconds_per_second / 2) /
                          nanoseconds_per_second;
    return checked_add(whole, fraction);
}

std::int64_t ns_to_samples_floor(std::int64_t nanoseconds, int sample_rate) {
    require_rescale_arguments(nanoseconds, sample_rate);
    const auto rate = static_cast<std::int64_t>(sample_rate);
    const auto whole = checked_quotient_product(nanoseconds / nanoseconds_per_second, rate);
    const auto remainder = nanoseconds % nanoseconds_per_second;
    return checked_add(whole, (remainder * rate) / nanoseconds_per_second);
}

AudioPtsClock::AudioPtsClock(std::int64_t first_anchor_ns,
                             std::int64_t origin_ns,
                             int sample_rate) {
    if (first_anchor_ns < origin_ns) {
        invalid_timing("audio timestamp precedes the output timeline origin");
    }
    current_pts_ = ns_to_samples_nearest(
        checked_subtract(first_anchor_ns, origin_ns), sample_rate);
}

void AudioPtsClock::advance(std::int64_t samples) {
    if (samples < 0) invalid_timing("audio sample count must not be negative");
    current_pts_ = checked_add(current_pts_, samples);
    samples_written_ = checked_add(samples_written_, samples);
}

AudioTimingResult analyze_and_normalize_audio(const AudioInfo& audio) {
    if (audio.sample_rate <= 0) invalid_timing("audio sample rate must be positive");
    if (audio.channels != 1 && audio.channels != 2) {
        invalid_timing("unsupported audio channel count");
    }

    AudioTimingResult result;
    result.normalized_chunks = audio.chunks;
    if (audio.chunks.empty()) return result;

    result.first_source_anchor_ns = audio.chunks.front().timestamp_ns;
    std::vector<std::int64_t> positive_chunk_samples;
    positive_chunk_samples.reserve(audio.chunks.size());
    std::int64_t previous_source_timestamp = 0;
    bool has_previous = false;

    for (std::size_t i = 0; i < audio.chunks.size(); ++i) {
        const auto& source = audio.chunks[i];
        auto& normalized = result.normalized_chunks[i];
        if (source.timestamp_ns < 0) {
            invalid_timing("audio source timestamp must not be negative");
        }
        if (source.interleaved_samples.empty() ||
            source.interleaved_samples.size() % static_cast<std::size_t>(audio.channels) != 0U) {
            invalid_timing("audio chunk must contain channel-aligned PCM samples");
        }

        const auto chunk_samples_size = source.interleaved_samples.size() /
                                        static_cast<std::size_t>(audio.channels);
        if (chunk_samples_size > static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max())) {
            invalid_timing("audio sample count exceeds the supported integer range");
        }
        const auto chunk_samples = static_cast<std::int64_t>(chunk_samples_size);
        positive_chunk_samples.push_back(chunk_samples);

        if (has_previous) {
            const auto delta = checked_subtract(source.timestamp_ns, previous_source_timestamp);
            if (delta <= 0) {
                ++result.diagnostics.source_non_monotonic_steps;
                if (delta < 0) {
                    result.diagnostics.source_max_backward_step_ns = std::max(
                        result.diagnostics.source_max_backward_step_ns,
                        absolute_checked(delta));
                }
            }
        }
        previous_source_timestamp = source.timestamp_ns;
        has_previous = true;

        const auto expected = checked_add(
            result.first_source_anchor_ns,
            samples_to_ns_nearest(result.cumulative_samples, audio.sample_rate));
        const auto residual = checked_subtract(source.timestamp_ns, expected);
        result.diagnostics.source_max_abs_residual_ns = std::max(
            result.diagnostics.source_max_abs_residual_ns,
            absolute_checked(residual));
        result.diagnostics.source_end_residual_ns = residual;
        normalized.timestamp_ns = expected;
        result.cumulative_samples = checked_add(result.cumulative_samples, chunk_samples);
    }

    const auto median = positive_chunk_samples.begin() +
                        static_cast<std::ptrdiff_t>(positive_chunk_samples.size() / 2U);
    std::nth_element(positive_chunk_samples.begin(), median, positive_chunk_samples.end());
    result.diagnostics.nominal_chunk_interval_ns =
        samples_to_ns_nearest(*median, audio.sample_rate);

    if (result.diagnostics.source_non_monotonic_steps > 0U) {
        result.diagnostics.warning_reasons.emplace_back("source_non_monotonic");
    }
    if (exceeds_twice(result.diagnostics.source_max_abs_residual_ns,
                      result.diagnostics.nominal_chunk_interval_ns)) {
        result.diagnostics.warning_reasons.emplace_back(
            "residual_exceeds_two_chunk_intervals");
    }
    return result;
}

std::int64_t audio_end_timestamp_ns(const std::vector<AudioChunk>& normalized_chunks,
                                    int sample_rate,
                                    int channels) {
    if (normalized_chunks.empty()) return 0;
    if (channels != 1 && channels != 2) invalid_timing("unsupported audio channel count");
    const auto& last = normalized_chunks.back();
    if (last.interleaved_samples.size() % static_cast<std::size_t>(channels) != 0U) {
        invalid_timing("audio chunk must contain channel-aligned PCM samples");
    }
    const auto samples = last.interleaved_samples.size() /
                         static_cast<std::size_t>(channels);
    if (samples > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        invalid_timing("audio sample count exceeds the supported integer range");
    }
    return checked_add(last.timestamp_ns,
                       samples_to_ns_nearest(static_cast<std::int64_t>(samples), sample_rate));
}

nlohmann::json audio_timing_to_json(const AudioTimingResult& timing) {
    return {
        {"mode", audio_timing_mode},
        {"normalized", true},
        {"source_non_monotonic_steps", timing.diagnostics.source_non_monotonic_steps},
        {"source_max_backward_step_ns", timing.diagnostics.source_max_backward_step_ns},
        {"source_max_abs_residual_ns", timing.diagnostics.source_max_abs_residual_ns},
        {"source_end_residual_ns", timing.diagnostics.source_end_residual_ns},
        {"nominal_chunk_interval_ns", timing.diagnostics.nominal_chunk_interval_ns},
        {"warning_reasons", timing.diagnostics.warning_reasons}
    };
}

} // namespace mcraw
