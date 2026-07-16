#include <motioncam/Decoder.hpp>
#include <motioncam/RawData.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <mcraw/io/mcraw_reader.hpp>

namespace {

enum class Mode { decoder_only, load_frame };

struct PreloadedFrame {
    std::vector<std::uint8_t> payload;
    std::uint32_t width{};
    std::uint32_t height{};
    int compression{};
};

struct Result {
    double wall_seconds{};
    double cpu_seconds{};
    double mean_ms{};
    double p50_ms{};
    double p90_ms{};
    double p95_ms{};
    double p99_ms{};
    double max_ms{};
    std::uint64_t read_bytes{};
    std::uint64_t decoded_bytes{};
    std::uint64_t checksum{};
    std::uint64_t peak_working_set{};
};

#ifdef _WIN32
double filetime_seconds(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return static_cast<double>(ticks.QuadPart) / 10'000'000.0;
}

double process_cpu_seconds() {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) {
        throw std::runtime_error("GetProcessTimes failed");
    }
    return filetime_seconds(kernel) + filetime_seconds(user);
}

std::uint64_t process_read_bytes() {
    IO_COUNTERS counters{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &counters)) {
        throw std::runtime_error("GetProcessIoCounters failed");
    }
    return counters.ReadTransferCount;
}

std::uint64_t peak_working_set_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        throw std::runtime_error("GetProcessMemoryInfo failed");
    }
    return counters.PeakWorkingSetSize;
}
#else
double process_cpu_seconds() { return 0.0; }
std::uint64_t process_read_bytes() { return 0U; }
std::uint64_t peak_working_set_bytes() { return 0U; }
#endif

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto position = quantile * static_cast<double>(values.size() - 1U);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1U, values.size() - 1U);
    const auto fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

std::vector<std::size_t> parse_workers(std::string_view value) {
    std::vector<std::size_t> result;
    std::size_t start = 0;
    while (start < value.size()) {
        const auto comma = value.find(',', start);
        const auto token = value.substr(start, comma == std::string_view::npos
            ? value.size() - start : comma - start);
        const auto workers = static_cast<std::size_t>(std::stoull(std::string(token)));
        if (workers == 0U) throw std::runtime_error("worker count must be positive");
        result.push_back(workers);
        if (comma == std::string_view::npos) break;
        start = comma + 1U;
    }
    return result;
}

std::vector<PreloadedFrame> preload_frames(const mcraw::McrawReader& reader,
                                           bool include_payload) {
    std::vector<PreloadedFrame> frames;
    frames.reserve(reader.frames().size());
    for (std::size_t i = 0; i < reader.frames().size(); ++i) {
        const auto metadata = reader.normalized_metadata(i);
        PreloadedFrame frame;
        if (include_payload) frame.payload = reader.load_compressed_frame(i).bytes;
        frame.width = metadata.width;
        frame.height = metadata.height;
        frame.compression = metadata.compression_type;
        frames.push_back(std::move(frame));
    }
    return frames;
}

Result run_once(const std::filesystem::path& path,
                const std::vector<motioncam::Timestamp>& timestamps,
                const std::vector<PreloadedFrame>& preloaded,
                Mode mode,
                std::size_t worker_count) {
    std::vector<std::unique_ptr<motioncam::Decoder>> decoders;
    if (mode == Mode::load_frame) {
        decoders.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            decoders.push_back(std::make_unique<motioncam::Decoder>(path.string()));
        }
    }

    std::vector<double> latencies(timestamps.size());
    std::vector<std::uint64_t> checksums(worker_count);
    std::atomic<std::size_t> next_frame{0};
    std::mutex start_mutex;
    std::condition_variable start_cv;
    std::size_t ready = 0;
    bool start = false;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            std::vector<std::uint8_t> decoded_bytes;
            std::vector<std::uint16_t> decoded_u16;
            nlohmann::json metadata;
            {
                std::unique_lock lock(start_mutex);
                ++ready;
                start_cv.notify_all();
                start_cv.wait(lock, [&] { return start; });
            }
            for (;;) {
                const auto index = next_frame.fetch_add(1U, std::memory_order_relaxed);
                if (index >= timestamps.size()) break;
                const auto begin = std::chrono::steady_clock::now();
                if (mode == Mode::load_frame) {
                    decoders[worker]->loadFrame(timestamps[index], decoded_bytes, metadata);
                    if (decoded_bytes.empty()) throw std::runtime_error("empty official RAW output");
                    checksums[worker] += decoded_bytes.front();
                    checksums[worker] += decoded_bytes[decoded_bytes.size() / 2U];
                } else {
                    const auto& frame = preloaded[index];
                    const auto pixels = static_cast<std::size_t>(frame.width) * frame.height;
                    decoded_u16.resize(pixels);
                    const auto decoded = frame.compression == 7
                        ? motioncam::raw::Decode(decoded_u16.data(),
                              static_cast<int>(frame.width), static_cast<int>(frame.height),
                              frame.payload.data(), frame.payload.size())
                        : motioncam::raw::DecodeLegacy(decoded_u16.data(),
                              static_cast<int>(frame.width), static_cast<int>(frame.height),
                              frame.payload.data(), frame.payload.size());
                    if (decoded == 0U) throw std::runtime_error("official RAW decode failed");
                    checksums[worker] += decoded_u16.front();
                    checksums[worker] += decoded_u16[decoded_u16.size() / 2U];
                }
                latencies[index] = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - begin).count();
            }
        });
    }

    {
        std::unique_lock lock(start_mutex);
        start_cv.wait(lock, [&] { return ready == worker_count; });
    }
    const auto cpu_start = process_cpu_seconds();
    const auto read_start = process_read_bytes();
    const auto wall_start = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock(start_mutex);
        start = true;
    }
    start_cv.notify_all();
    for (auto& worker : workers) worker.join();
    const auto wall_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_start).count();

    Result result;
    result.wall_seconds = wall_seconds;
    result.cpu_seconds = process_cpu_seconds() - cpu_start;
    result.mean_ms = std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                     static_cast<double>(latencies.size());
    result.p50_ms = percentile(latencies, 0.50);
    result.p90_ms = percentile(latencies, 0.90);
    result.p95_ms = percentile(latencies, 0.95);
    result.p99_ms = percentile(latencies, 0.99);
    result.max_ms = *std::max_element(latencies.begin(), latencies.end());
    result.read_bytes = process_read_bytes() - read_start;
    result.decoded_bytes = std::accumulate(preloaded.begin(), preloaded.end(), std::uint64_t{},
        [](std::uint64_t total, const PreloadedFrame& frame) {
            return total + static_cast<std::uint64_t>(frame.width) * frame.height * 2U;
        });
    result.checksum = std::accumulate(checksums.begin(), checksums.end(), std::uint64_t{});
    result.peak_working_set = peak_working_set_bytes();
    return result;
}

} // namespace

int main(int argc, char** argv) try {
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: mcraw-decoder-benchmark INPUT MODE [WORKERS] [PASSES]\n"
                     "  MODE: decoder-only | load-frame\n";
        return 2;
    }
    const std::filesystem::path path{argv[1]};
    const std::string_view mode_text{argv[2]};
    const Mode mode = mode_text == "decoder-only" ? Mode::decoder_only
        : mode_text == "load-frame" ? Mode::load_frame
        : throw std::runtime_error("mode must be decoder-only or load-frame");
    const auto worker_counts = parse_workers(argc >= 4 ? argv[3] : "1,2,4,6,8");
    const int passes = argc >= 5 ? std::stoi(argv[4]) : 3;
    if (passes <= 0) throw std::runtime_error("passes must be positive");

    mcraw::McrawReader reader(path);
    motioncam::Decoder probe(path.string());
    const auto timestamps = probe.getFrames();
    if (timestamps.empty()) throw std::runtime_error("input has no frames");
    const auto preloaded = preload_frames(reader, mode == Mode::decoder_only);
    if (preloaded.size() != timestamps.size()) throw std::runtime_error("frame index mismatch");
    const auto compression = preloaded.front().compression;
    if (!std::all_of(preloaded.begin(), preloaded.end(), [compression](const auto& frame) {
            return frame.compression == compression;
        })) throw std::runtime_error("mixed compression types are not benchmarkable as one corpus");

    std::cout << "mode,compression,workers,pass,frames,wall_s,fps,mp_s,cpu_s,"
                 "cpu_core_equiv,mean_ms,p50_ms,p90_ms,p95_ms,p99_ms,max_ms,"
                 "read_GB,read_GBps,decoded_GB,peak_working_set_MiB,checksum\n";
    std::cout << std::fixed << std::setprecision(3);
    for (const auto workers : worker_counts) {
        static_cast<void>(run_once(path, timestamps, preloaded, mode, workers));
        for (int pass = 1; pass <= passes; ++pass) {
            const auto result = run_once(path, timestamps, preloaded, mode, workers);
            const auto fps = static_cast<double>(timestamps.size()) / result.wall_seconds;
            const auto megapixels = static_cast<double>(preloaded.front().width) *
                preloaded.front().height * timestamps.size() / 1.0e6;
            const auto read_gb = static_cast<double>(result.read_bytes) / 1.0e9;
            const auto decoded_gb = static_cast<double>(result.decoded_bytes) / 1.0e9;
            std::cout << mode_text << ',' << compression << ',' << workers << ',' << pass << ','
                      << timestamps.size() << ',' << result.wall_seconds << ',' << fps << ','
                      << megapixels / result.wall_seconds << ',' << result.cpu_seconds << ','
                      << result.cpu_seconds / result.wall_seconds << ',' << result.mean_ms << ','
                      << result.p50_ms << ',' << result.p90_ms << ',' << result.p95_ms << ','
                      << result.p99_ms << ',' << result.max_ms << ',' << read_gb << ','
                      << read_gb / result.wall_seconds << ',' << decoded_gb << ','
                      << static_cast<double>(result.peak_working_set) / (1024.0 * 1024.0) << ','
                      << result.checksum << std::endl;
        }
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
