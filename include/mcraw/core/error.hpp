#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace mcraw {

enum class ErrorCode {
    invalid_argument,
    invalid_container,
    invalid_metadata,
    unsupported_format,
    decode_failed,
    processing_failed,
    encode_failed,
    io_failed,
    device_lost,
};

[[nodiscard]] constexpr const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::invalid_container: return "invalid_container";
    case ErrorCode::invalid_metadata: return "invalid_metadata";
    case ErrorCode::unsupported_format: return "unsupported_format";
    case ErrorCode::decode_failed: return "decode_failed";
    case ErrorCode::processing_failed: return "processing_failed";
    case ErrorCode::encode_failed: return "encode_failed";
    case ErrorCode::io_failed: return "io_failed";
    case ErrorCode::device_lost: return "device_lost";
    }
    return "unknown";
}

class Error final : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

} // namespace mcraw
