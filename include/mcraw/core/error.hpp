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
};

class Error final : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

} // namespace mcraw
