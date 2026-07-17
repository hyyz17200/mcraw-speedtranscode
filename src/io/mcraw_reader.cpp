#include <mcraw/io/mcraw_reader.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include <motioncam/Container.hpp>
#include <motioncam/Decoder.hpp>
#include <nlohmann/json.hpp>

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

template <typename T>
void read_exact(std::ifstream& stream, T& value, std::string_view what) {
    stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!stream) throw Error(ErrorCode::invalid_container, "failed to read " + std::string(what));
}

void validate_index_count(std::int32_t count, std::uintmax_t file_size) {
    if (count < 0 || static_cast<std::uintmax_t>(count) > file_size / sizeof(motioncam::BufferOffset)) {
        throw Error(ErrorCode::invalid_container, "invalid frame index count");
    }
}

} // namespace

class McrawReader::Impl {
public:
    explicit Impl(std::filesystem::path input)
        : path(std::move(input)),
          primary_decoder(std::make_unique<motioncam::Decoder>(path.string())) {
        container_metadata_value = primary_decoder->getContainerMetadata();
        parse_index();
        idle_decoders.push_back(std::move(primary_decoder));
    }

    // Official decoders keep per-instance file handles and scratch buffers,
    // so concurrent frames each lease their own instance instead of
    // serializing every decode behind one shared decoder.
    class DecoderLease {
    public:
        explicit DecoderLease(Impl& owner) : owner_(owner) {
            {
                std::scoped_lock lock(owner_.decoder_pool_mutex);
                if (!owner_.idle_decoders.empty()) {
                    decoder_ = std::move(owner_.idle_decoders.back());
                    owner_.idle_decoders.pop_back();
                }
            }
            if (decoder_ == nullptr) {
                decoder_ = std::make_unique<motioncam::Decoder>(owner_.path.string());
            }
        }

        ~DecoderLease() {
            std::scoped_lock lock(owner_.decoder_pool_mutex);
            owner_.idle_decoders.push_back(std::move(decoder_));
        }

        DecoderLease(const DecoderLease&) = delete;
        DecoderLease& operator=(const DecoderLease&) = delete;

        [[nodiscard]] motioncam::Decoder& get() noexcept { return *decoder_; }

    private:
        Impl& owner_;
        std::unique_ptr<motioncam::Decoder> decoder_;
    };

    void parse_index() {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw Error(ErrorCode::io_failed, "cannot open MCRAW file: " + path.string());

        motioncam::Header header{};
        read_exact(stream, header, "container header");
        if (std::memcmp(header.ident, motioncam::CONTAINER_ID, sizeof(header.ident)) != 0) {
            throw Error(ErrorCode::invalid_container, "invalid MCRAW identifier");
        }
        version = header.version;
        if (version != motioncam::CONTAINER_VERSION) {
            throw Error(ErrorCode::unsupported_format, "unsupported MCRAW container version");
        }

        const auto file_size = std::filesystem::file_size(path);
        constexpr auto footer_size = sizeof(motioncam::Item) + sizeof(motioncam::BufferIndex);
        if (file_size < footer_size) {
            throw Error(ErrorCode::invalid_container, "MCRAW file is too small to contain an index");
        }
        stream.seekg(-static_cast<std::streamoff>(footer_size), std::ios::end);
        motioncam::Item item{};
        motioncam::BufferIndex index{};
        read_exact(stream, item, "frame index item");
        read_exact(stream, index, "frame index footer");
        if (item.type != motioncam::Type::BUFFER_INDEX ||
            static_cast<std::uint32_t>(index.magicNumber) != motioncam::INDEX_MAGIC_NUMBER) {
            throw Error(ErrorCode::invalid_container, "missing or corrupt MCRAW frame index");
        }
        validate_index_count(index.numOffsets, file_size);
        if (index.indexDataOffset < 0 || static_cast<std::uintmax_t>(index.indexDataOffset) >= file_size) {
            throw Error(ErrorCode::invalid_container, "frame index data offset is out of range");
        }

        stream.seekg(index.indexDataOffset, std::ios::beg);
        std::vector<motioncam::BufferOffset> offsets(static_cast<std::size_t>(index.numOffsets));
        if (!offsets.empty()) {
            stream.read(reinterpret_cast<char*>(offsets.data()),
                        static_cast<std::streamsize>(offsets.size() * sizeof(offsets.front())));
            if (!stream) throw Error(ErrorCode::invalid_container, "truncated MCRAW frame index");
        }
        std::sort(offsets.begin(), offsets.end(), [](const auto& a, const auto& b) {
            return a.timestamp < b.timestamp;
        });
        records.reserve(offsets.size());
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            if (offsets[i].offset < 0 || static_cast<std::uintmax_t>(offsets[i].offset) >= file_size) {
                throw Error(ErrorCode::invalid_container, "frame payload offset is out of range");
            }
            if (i > 0 && offsets[i].timestamp <= offsets[i - 1].timestamp) {
                throw Error(ErrorCode::invalid_container, "frame timestamps are not strictly increasing");
            }
            records.push_back({i, offsets[i].timestamp, offsets[i].offset});
        }

        if (primary_decoder->getFrames().size() != records.size()) {
            throw Error(ErrorCode::invalid_container, "official decoder and independent index disagree on frame count");
        }
    }

    const FrameRecord& checked_frame(std::size_t index) const {
        if (index >= records.size()) {
            throw Error(ErrorCode::invalid_argument, "frame index is out of range");
        }
        return records[index];
    }

    std::filesystem::path path;
    std::unique_ptr<motioncam::Decoder> primary_decoder;
    nlohmann::json container_metadata_value;
    std::uint8_t version{};
    std::vector<FrameRecord> records;
    mutable std::mutex decoder_pool_mutex;
    mutable std::vector<std::unique_ptr<motioncam::Decoder>> idle_decoders;
};

McrawReader::McrawReader(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {}
McrawReader::~McrawReader() = default;
McrawReader::McrawReader(McrawReader&&) noexcept = default;
McrawReader& McrawReader::operator=(McrawReader&&) noexcept = default;

std::uint8_t McrawReader::container_version() const noexcept { return impl_->version; }
const std::filesystem::path& McrawReader::path() const noexcept { return impl_->path; }
const std::vector<FrameRecord>& McrawReader::frames() const noexcept { return impl_->records; }

const nlohmann::json& McrawReader::container_metadata() const {
    return impl_->container_metadata_value;
}

nlohmann::json McrawReader::frame_metadata(std::size_t index) const {
    const auto& record = impl_->checked_frame(index);
    nlohmann::json result;
    Impl::DecoderLease lease(*impl_);
    lease.get().loadFrameMetadata(record.timestamp_ns, result);
    return result;
}

NormalizedCameraMetadata McrawReader::normalized_metadata(std::size_t index) const {
    return normalize_metadata(container_metadata(), frame_metadata(index));
}

CompressedFrame McrawReader::load_compressed_frame(std::size_t index) const {
    const auto& record = impl_->checked_frame(index);
    std::ifstream stream(impl_->path, std::ios::binary);
    if (!stream) throw Error(ErrorCode::io_failed, "cannot reopen MCRAW file");
    stream.seekg(record.file_offset, std::ios::beg);
    motioncam::Item item{};
    read_exact(stream, item, "compressed frame item");
    if (item.type != motioncam::Type::BUFFER) {
        throw Error(ErrorCode::invalid_container, "frame index does not point to a BUFFER item");
    }
    const auto file_size = std::filesystem::file_size(impl_->path);
    if (item.size == 0U || item.size > file_size) {
        throw Error(ErrorCode::invalid_container, "compressed frame payload length is invalid");
    }
    CompressedFrame result;
    result.timestamp_ns = record.timestamp_ns;
    result.bytes.resize(item.size);
    stream.read(reinterpret_cast<char*>(result.bytes.data()), static_cast<std::streamsize>(result.bytes.size()));
    if (!stream) throw Error(ErrorCode::invalid_container, "compressed frame payload is truncated");
    return result;
}

RawMosaicU16 McrawReader::load_reference_frame(std::size_t index) const {
    return std::move(load_reference_frame_with_metadata(index).raw);
}

DecodedRawFrame McrawReader::load_reference_frame_with_metadata(std::size_t index) const {
    const auto& record = impl_->checked_frame(index);
    std::vector<std::uint8_t> decoded_bytes;
    nlohmann::json frame_metadata;
    {
        Impl::DecoderLease lease(*impl_);
        lease.get().loadFrame(record.timestamp_ns, decoded_bytes, frame_metadata);
    }
    const auto metadata = normalize_metadata(container_metadata(), frame_metadata);
    const auto pixels = static_cast<std::size_t>(metadata.width) * metadata.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t) ||
        decoded_bytes.size() != pixels * sizeof(std::uint16_t)) {
        throw Error(ErrorCode::decode_failed, "official decoder returned an unexpected RAW buffer size");
    }
    std::vector<std::uint16_t> decoded(pixels);
    std::memcpy(decoded.data(), decoded_bytes.data(), decoded_bytes.size());
    RawMosaicU16 raw{metadata.width, metadata.height, metadata.cfa, std::move(decoded)};
    raw.validate();
    return {std::move(raw), std::move(metadata)};
}

AudioInfo McrawReader::load_audio() const {
    AudioInfo result;
    const auto& metadata = impl_->container_metadata_value;
    const auto extra = metadata.find("extraData");
    if (extra == metadata.end() || !extra->is_object() ||
        !extra->contains("audioSampleRate") || !extra->contains("audioChannels")) {
        return result;
    }
    const auto& sample_rate = extra->at("audioSampleRate");
    const auto& channels = extra->at("audioChannels");
    if (!sample_rate.is_number_integer() || !channels.is_number_integer()) {
        throw Error(ErrorCode::invalid_metadata, "audio sample rate and channels must be integers");
    }
    result.sample_rate = sample_rate.get<int>();
    result.channels = channels.get<int>();
    if (result.sample_rate <= 0 || (result.channels != 1 && result.channels != 2)) {
        throw Error(ErrorCode::invalid_metadata, "unsupported or invalid audio format");
    }

    std::ifstream stream(impl_->path, std::ios::binary);
    if (!stream) throw Error(ErrorCode::io_failed, "cannot reopen MCRAW file for audio");
    const auto file_size = std::filesystem::file_size(impl_->path);
    const auto last_physical_frame = std::max_element(
        impl_->records.begin(), impl_->records.end(),
        [](const FrameRecord& a, const FrameRecord& b) { return a.file_offset < b.file_offset; });
    if (last_physical_frame == impl_->records.end()) return result;

    std::vector<motioncam::BufferOffset> audio_offsets;
    auto position = static_cast<std::uintmax_t>(last_physical_frame->file_offset);
    while (position + sizeof(motioncam::Item) <= file_size) {
        stream.seekg(static_cast<std::streamoff>(position), std::ios::beg);
        motioncam::Item item{};
        read_exact(stream, item, "tail item while locating audio index");
        const auto payload = position + sizeof(item);
        if (item.size > file_size - payload) {
            throw Error(ErrorCode::invalid_container, "tail item extends beyond the MCRAW file");
        }
        if (item.type == motioncam::Type::AUDIO_INDEX) {
            if (item.size < sizeof(motioncam::AudioIndex)) {
                throw Error(ErrorCode::invalid_container, "audio index payload is too small");
            }
            motioncam::AudioIndex index{};
            read_exact(stream, index, "audio index header");
            const auto available = (item.size - sizeof(index)) / sizeof(motioncam::BufferOffset);
            if (index.numOffsets < 0 || static_cast<std::uint64_t>(index.numOffsets) > available) {
                throw Error(ErrorCode::invalid_container, "invalid audio index count");
            }
            audio_offsets.resize(static_cast<std::size_t>(index.numOffsets));
            if (!audio_offsets.empty()) {
                stream.read(reinterpret_cast<char*>(audio_offsets.data()),
                            static_cast<std::streamsize>(audio_offsets.size() * sizeof(audio_offsets.front())));
                if (!stream) throw Error(ErrorCode::invalid_container, "truncated audio index");
            }
            break;
        }
        position = payload + item.size;
    }
    if (audio_offsets.empty()) {
        throw Error(ErrorCode::invalid_container, "audio metadata is present but no audio index was found");
    }

    result.chunks.reserve(audio_offsets.size());
    for (const auto& offset : audio_offsets) {
        if (offset.offset < 0 || static_cast<std::uintmax_t>(offset.offset) + sizeof(motioncam::Item) > file_size) {
            throw Error(ErrorCode::invalid_container, "audio payload offset is out of range");
        }
        stream.seekg(offset.offset, std::ios::beg);
        motioncam::Item data_item{};
        read_exact(stream, data_item, "audio data item");
        if (data_item.type != motioncam::Type::AUDIO_DATA || data_item.size == 0U ||
            (data_item.size % sizeof(std::int16_t)) != 0U ||
            data_item.size > file_size - static_cast<std::uintmax_t>(offset.offset) - sizeof(data_item)) {
            throw Error(ErrorCode::invalid_container, "invalid audio PCM payload");
        }
        AudioChunk chunk;
        chunk.interleaved_samples.resize(data_item.size / sizeof(std::int16_t));
        stream.read(reinterpret_cast<char*>(chunk.interleaved_samples.data()),
                    static_cast<std::streamsize>(data_item.size));
        if (!stream) throw Error(ErrorCode::invalid_container, "truncated audio PCM payload");

        motioncam::Item timestamp_item{};
        read_exact(stream, timestamp_item, "audio timestamp item");
        if (timestamp_item.type != motioncam::Type::AUDIO_DATA_METADATA ||
            timestamp_item.size < sizeof(motioncam::AudioMetadata)) {
            throw Error(ErrorCode::invalid_container, "audio chunk has no source timestamp metadata");
        }
        motioncam::AudioMetadata timestamp{};
        read_exact(stream, timestamp, "audio source timestamp");
        chunk.timestamp_ns = timestamp.timestampNs;
        if (chunk.interleaved_samples.size() % static_cast<std::size_t>(result.channels) != 0U) {
            throw Error(ErrorCode::invalid_container, "audio chunk is not channel-aligned");
        }
        result.chunks.push_back(std::move(chunk));
    }
    return result;
}

} // namespace mcraw
