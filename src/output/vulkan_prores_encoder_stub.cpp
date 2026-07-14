#include <mcraw/output/vulkan_prores_encoder_stub.hpp>

#include <utility>

#include <mcraw/core/error.hpp>

namespace mcraw {

VulkanProResEncoderStub::VulkanProResEncoderStub(std::string unavailable_reason)
    : unavailable_reason_(std::move(unavailable_reason)) {}

VideoEncoderCapabilities VulkanProResEncoderStub::capabilities() const {
    return {"prores_ks_vulkan", FrameStorage::vulkan, false, true, unavailable_reason_};
}

void VulkanProResEncoderStub::send(VideoFrame) { fail(); }
std::vector<EncodedPacket> VulkanProResEncoderStub::drain() { fail(); }
std::vector<EncodedPacket> VulkanProResEncoderStub::flush() { fail(); }

[[noreturn]] void VulkanProResEncoderStub::fail() const {
    throw Error(ErrorCode::unsupported_format,
                "Vulkan ProRes encoder is unavailable: " + unavailable_reason_);
}

} // namespace mcraw
