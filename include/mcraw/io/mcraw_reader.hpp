#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <mcraw/core/metadata.hpp>
#include <mcraw/core/pixel_types.hpp>

namespace mcraw {

struct FrameRecord {
    std::size_t index{};
    std::int64_t timestamp_ns{};
    std::int64_t file_offset{};
};

struct AudioChunk {
    std::int64_t timestamp_ns{-1};
    std::vector<std::int16_t> interleaved_samples;
};

struct AudioInfo {
    int sample_rate{};
    int channels{};
    std::vector<AudioChunk> chunks;
};

struct DecodedRawFrame {
    RawMosaicU16 raw;
    NormalizedCameraMetadata metadata;
};

class McrawReader {
public:
    explicit McrawReader(const std::filesystem::path& path);
    ~McrawReader();
    McrawReader(McrawReader&&) noexcept;
    McrawReader& operator=(McrawReader&&) noexcept;
    McrawReader(const McrawReader&) = delete;
    McrawReader& operator=(const McrawReader&) = delete;

    [[nodiscard]] std::uint8_t container_version() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const std::vector<FrameRecord>& frames() const noexcept;
    [[nodiscard]] const nlohmann::json& container_metadata() const;
    [[nodiscard]] nlohmann::json frame_metadata(std::size_t index) const;
    [[nodiscard]] NormalizedCameraMetadata normalized_metadata(std::size_t index) const;
    [[nodiscard]] CompressedFrame load_compressed_frame(std::size_t index) const;
    [[nodiscard]] RawMosaicU16 load_reference_frame(std::size_t index) const;
    [[nodiscard]] DecodedRawFrame load_reference_frame_with_metadata(std::size_t index) const;
    [[nodiscard]] AudioInfo load_audio() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcraw
