#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <mcraw/core/timing.hpp>
#include <mcraw/core/audio_timing.hpp>
#include <mcraw/core/error.hpp>

TEST_CASE("stage timing reports deterministic percentiles") {
    mcraw::StageTimings timings;
    for (int value = 1; value <= 100; ++value) timings.add("stage", static_cast<double>(value));
    const auto summary = timings.summary("stage");
    REQUIRE(summary.samples == 100);
    REQUIRE(summary.mean_ms == Catch::Approx(50.5));
    REQUIRE(summary.p50_ms == Catch::Approx(50.5));
    REQUIRE(summary.p95_ms == Catch::Approx(95.05));
}

namespace {

mcraw::AudioChunk audio_chunk(std::int64_t timestamp_ns,
                              std::size_t samples_per_channel,
                              int channels = 2) {
    return {timestamp_ns, std::vector<std::int16_t>(
        samples_per_channel * static_cast<std::size_t>(channels), 0)};
}

} // namespace

TEST_CASE("audio integer rescale uses nearest and floor at sample boundaries") {
    constexpr int rate = 48'000;
    REQUIRE(mcraw::samples_to_ns_nearest(1, rate) == 20'833);
    REQUIRE(mcraw::ns_to_samples_floor(20'832, rate) == 0);
    REQUIRE(mcraw::ns_to_samples_floor(20'833, rate) == 0);
    REQUIRE(mcraw::ns_to_samples_floor(20'834, rate) == 1);
    REQUIRE(mcraw::ns_to_samples_floor(20'833'333, rate) == 999);
    REQUIRE(mcraw::ns_to_samples_floor(20'833'334, rate) == 1'000);
    REQUIRE(mcraw::ns_to_samples_nearest(10'417, rate) == 1);
}

TEST_CASE("audio timing normalizes routine jitter without warning") {
    mcraw::AudioInfo audio{48'000, 2, {
        audio_chunk(1'000'000'000, 1'024),
        audio_chunk(1'021'000'000, 1'024),
        audio_chunk(1'043'000'000, 1'024)
    }};
    const auto original = audio.chunks;
    const auto timing = mcraw::analyze_and_normalize_audio(audio);
    REQUIRE_FALSE(timing.should_warn());
    REQUIRE(timing.first_source_anchor_ns == 1'000'000'000);
    REQUIRE(timing.cumulative_samples == 3'072);
    REQUIRE(timing.normalized_chunks[1].timestamp_ns == 1'021'333'333);
    REQUIRE(timing.normalized_chunks[2].timestamp_ns == 1'042'666'667);
    REQUIRE(timing.diagnostics.source_non_monotonic_steps == 0);
    REQUIRE(timing.diagnostics.nominal_chunk_interval_ns == 21'333'333);
    for (std::size_t i = 0; i < original.size(); ++i) {
        REQUIRE(timing.normalized_chunks[i].interleaved_samples ==
                original[i].interleaved_samples);
    }
}

TEST_CASE("audio timing reports backward and duplicate source timestamps") {
    SECTION("backward") {
        mcraw::AudioInfo audio{48'000, 2, {
            audio_chunk(1'000'000'000, 1'024),
            audio_chunk(999'999'666, 1'024)
        }};
        const auto timing = mcraw::analyze_and_normalize_audio(audio);
        REQUIRE(timing.diagnostics.source_non_monotonic_steps == 1);
        REQUIRE(timing.diagnostics.source_max_backward_step_ns == 334);
        REQUIRE(timing.diagnostics.warning_reasons ==
                std::vector<std::string>{"source_non_monotonic"});
    }
    SECTION("duplicate") {
        mcraw::AudioInfo audio{48'000, 2, {
            audio_chunk(1'000'000'000, 1'024),
            audio_chunk(1'000'000'000, 1'024)
        }};
        const auto timing = mcraw::analyze_and_normalize_audio(audio);
        REQUIRE(timing.diagnostics.source_non_monotonic_steps == 1);
        REQUIRE(timing.diagnostics.source_max_backward_step_ns == 0);
        REQUIRE(timing.should_warn());
    }
}

TEST_CASE("audio timing warns only above two nominal intervals") {
    mcraw::AudioInfo below{48'000, 2, {
        audio_chunk(1'000'000'000, 1'024),
        audio_chunk(1'021'333'333 + 42'666'666, 1'024)
    }};
    REQUIRE_FALSE(mcraw::analyze_and_normalize_audio(below).should_warn());

    mcraw::AudioInfo above{48'000, 2, {
        audio_chunk(1'000'000'000, 1'024),
        audio_chunk(1'021'333'333 + 42'666'667, 1'024)
    }};
    const auto timing = mcraw::analyze_and_normalize_audio(above);
    REQUIRE(timing.diagnostics.warning_reasons ==
            std::vector<std::string>{"residual_exceeds_two_chunk_intervals"});
}

TEST_CASE("audio timing rejects negative timestamps in the shared operation") {
    mcraw::AudioInfo audio{48'000, 2, {
        audio_chunk(1'000'000'000, 1'024),
        audio_chunk(-1, 1'024)
    }};
    REQUIRE_THROWS_AS(mcraw::analyze_and_normalize_audio(audio), mcraw::Error);
}

TEST_CASE("audio end uses the integer sample clock") {
    const std::vector<mcraw::AudioChunk> chunks{
        audio_chunk(1'000'000'000, 1'024),
        audio_chunk(1'021'333'333, 1'024)
    };
    REQUIRE(mcraw::audio_end_timestamp_ns(chunks, 48'000, 2) == 1'042'666'666);
}
