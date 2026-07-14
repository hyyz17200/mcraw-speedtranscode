#include <mcraw/vulkan/vulkan_runtime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <memory>
#include <utility>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/opt.h>
}

#include <mcraw/core/error.hpp>

namespace mcraw {
namespace {

struct InstanceDeleter {
    void operator()(VkInstance_T* instance) const noexcept {
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }
};
using InstancePtr = std::unique_ptr<VkInstance_T, InstanceDeleter>;

std::string lowercase(std::string_view value) {
    std::string output(value);
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return output;
}

std::string compact_uuid(const std::uint8_t* bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.resize(VK_UUID_SIZE * 2U);
    for (std::size_t i = 0; i < VK_UUID_SIZE; ++i) {
        output[i * 2U] = hex[bytes[i] >> 4U];
        output[i * 2U + 1U] = hex[bytes[i] & 0x0fU];
    }
    return output;
}

std::string normalize_uuid(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (std::isxdigit(character) != 0) {
            result.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return result;
}

bool looks_like_software(const VkPhysicalDeviceProperties& properties) {
    const auto name = lowercase(properties.deviceName);
    return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
           name.find("microsoft basic render") != std::string::npos ||
           name.find("swiftshader") != std::string::npos ||
           name.find("llvmpipe") != std::string::npos;
}

InstancePtr create_enumeration_instance() {
    std::uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
        const auto result = vkEnumerateInstanceVersion(&loader_version);
        if (result != VK_SUCCESS) {
            throw Error(ErrorCode::processing_failed,
                        "vkEnumerateInstanceVersion failed: " + std::to_string(result));
        }
    }
    if (loader_version < VK_API_VERSION_1_3) {
        throw Error(ErrorCode::unsupported_format, "Vulkan 1.3 or newer is required");
    }
    const VkApplicationInfo application{
        VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "mcraw-transcoder", 1,
        "mcraw-vulkan-probe", 1, VK_API_VERSION_1_3};
    const VkInstanceCreateInfo create_info{
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &application, 0, nullptr, 0, nullptr};
    VkInstance instance = VK_NULL_HANDLE;
    const auto result = vkCreateInstance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        throw Error(ErrorCode::processing_failed,
                    "vkCreateInstance for device enumeration failed: " + std::to_string(result));
    }
    return InstancePtr(instance);
}

VulkanDeviceInfo inspect_device(VkPhysicalDevice physical_device, std::uint32_t index) {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    identity.pNext = &driver;
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &identity;
    vkGetPhysicalDeviceProperties2(physical_device, &properties);

    VulkanDeviceInfo result;
    result.enumeration_index = index;
    result.name = properties.properties.deviceName;
    result.type = properties.properties.deviceType;
    result.vendor_id = properties.properties.vendorID;
    result.device_id = properties.properties.deviceID;
    result.api_version = properties.properties.apiVersion;
    result.driver_version = properties.properties.driverVersion;
    result.driver_name = driver.driverName;
    result.driver_info = driver.driverInfo;
    result.uuid = compact_uuid(identity.deviceUUID);
    result.software = looks_like_software(properties.properties);

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
    result.queue_families.reserve(family_count);
    for (std::uint32_t family = 0; family < family_count; ++family) {
        result.queue_families.push_back(
            {family, families[family].queueCount, families[family].queueFlags});
    }
    return result;
}

bool has_compute_queue(const VulkanDeviceInfo& device) {
    return std::any_of(device.queue_families.begin(), device.queue_families.end(),
                       [](const VulkanQueueFamilyInfo& family) {
                           return family.queue_count > 0U &&
                                  (family.flags & VK_QUEUE_COMPUTE_BIT) != 0U;
                       });
}

VulkanDeviceInfo require_compute_device(const VulkanDeviceInfo& device) {
    if (!has_compute_queue(device)) {
        throw Error(ErrorCode::unsupported_format,
                    "selected Vulkan device has no compute queue: " + device.name);
    }
    return device;
}

} // namespace

std::vector<VulkanDeviceInfo> VulkanRuntime::enumerate_devices() {
    const auto instance = create_enumeration_instance();
    std::uint32_t count = 0;
    auto result = vkEnumeratePhysicalDevices(instance.get(), &count, nullptr);
    if (result != VK_SUCCESS) {
        throw Error(ErrorCode::processing_failed,
                    "vkEnumeratePhysicalDevices failed: " + std::to_string(result));
    }
    if (count == 0U) return {};
    std::vector<VkPhysicalDevice> physical_devices(count);
    result = vkEnumeratePhysicalDevices(instance.get(), &count, physical_devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        throw Error(ErrorCode::processing_failed,
                    "vkEnumeratePhysicalDevices failed: " + std::to_string(result));
    }
    std::vector<VulkanDeviceInfo> devices;
    devices.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        devices.push_back(inspect_device(physical_devices[index], index));
    }
    return devices;
}

VulkanDeviceInfo VulkanRuntime::select_device(const std::vector<VulkanDeviceInfo>& devices,
                                              std::string_view selector) {
    if (devices.empty()) {
        throw Error(ErrorCode::unsupported_format, "no Vulkan physical devices were found");
    }
    if (selector.empty()) {
        throw Error(ErrorCode::invalid_argument, "Vulkan device selector must not be empty");
    }
    if (selector == "auto") {
        for (const auto preferred_type : {VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                                          VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                                          VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
                                          VK_PHYSICAL_DEVICE_TYPE_OTHER}) {
            const auto match = std::find_if(devices.begin(), devices.end(),
                [preferred_type](const VulkanDeviceInfo& device) {
                    return !device.software && device.type == preferred_type &&
                           has_compute_queue(device);
                });
            if (match != devices.end()) return require_compute_device(*match);
        }
        throw Error(ErrorCode::unsupported_format,
                    "no non-software Vulkan device with a compute queue was found");
    }

    std::uint32_t index = 0;
    const auto* begin = selector.data();
    const auto* end = begin + selector.size();
    const auto parsed = std::from_chars(begin, end, index);
    if (parsed.ec == std::errc{} && parsed.ptr == end) {
        const auto match = std::find_if(devices.begin(), devices.end(),
            [index](const VulkanDeviceInfo& device) { return device.enumeration_index == index; });
        if (match == devices.end()) {
            throw Error(ErrorCode::invalid_argument,
                        "Vulkan device index is not present: " + std::string(selector));
        }
        return require_compute_device(*match);
    }

    if (selector.starts_with("uuid:")) {
        const auto requested = normalize_uuid(selector.substr(5));
        const auto match = std::find_if(devices.begin(), devices.end(),
            [&requested](const VulkanDeviceInfo& device) { return device.uuid == requested; });
        if (match == devices.end()) {
            throw Error(ErrorCode::invalid_argument,
                        "Vulkan device UUID is not present: " + std::string(selector));
        }
        return require_compute_device(*match);
    }

    std::uint32_t vendor = 0;
    std::uint32_t device_id = 0;
    bool parsed_pci_id = false;
    if (selector.starts_with("pci:")) {
        const auto separator = selector.find(':', 4U);
        if (separator != std::string_view::npos) {
            const auto vendor_text = selector.substr(4U, separator - 4U);
            const auto device_text = selector.substr(separator + 1U);
            const auto vendor_result = std::from_chars(
                vendor_text.data(), vendor_text.data() + vendor_text.size(), vendor, 16);
            const auto device_result = std::from_chars(
                device_text.data(), device_text.data() + device_text.size(), device_id, 16);
            parsed_pci_id = vendor_result.ec == std::errc{} &&
                            vendor_result.ptr == vendor_text.data() + vendor_text.size() &&
                            device_result.ec == std::errc{} &&
                            device_result.ptr == device_text.data() + device_text.size();
        }
    }
    if (selector.starts_with("pci:") && !parsed_pci_id) {
        throw Error(ErrorCode::invalid_argument,
                    "invalid Vulkan vendor/device selector: " + std::string(selector));
    }
    if (parsed_pci_id) {
        const auto match = std::find_if(devices.begin(), devices.end(),
            [vendor, device_id](const VulkanDeviceInfo& device) {
                return device.vendor_id == vendor && device.device_id == device_id;
            });
        if (match == devices.end()) {
            throw Error(ErrorCode::invalid_argument,
                        "Vulkan vendor/device ID is not present: " + std::string(selector));
        }
        return require_compute_device(*match);
    }

    const auto requested_name = lowercase(selector);
    const auto match = std::find_if(devices.begin(), devices.end(),
        [&requested_name](const VulkanDeviceInfo& device) {
            return lowercase(device.name).find(requested_name) != std::string::npos;
        });
    if (match == devices.end()) {
        throw Error(ErrorCode::invalid_argument,
                    "Vulkan device name is not present: " + std::string(selector));
    }
    return require_compute_device(*match);
}

class VulkanRuntime::Impl {
public:
    explicit Impl(const VulkanRuntimeConfig& config) {
        const auto devices = VulkanRuntime::enumerate_devices();
        selected = VulkanRuntime::select_device(devices, config.selector);

        AVDictionary* options = nullptr;
        if (config.enable_validation) av_dict_set(&options, "debug", "validate", 0);
        AVBufferRef* raw_device = nullptr;
        const int create_result = av_hwdevice_ctx_create(
            &raw_device, AV_HWDEVICE_TYPE_VULKAN, selected.name.c_str(), options, 0);
        av_dict_free(&options);
        require_ffmpeg(create_result, "create FFmpeg-owned Vulkan device");
        device_context.reset(raw_device);

        auto* public_context = ffmpeg_device_context();
        vulkan_context = static_cast<AVVulkanDeviceContext*>(public_context->hwctx);
        if (vulkan_context == nullptr || vulkan_context->inst == VK_NULL_HANDLE ||
            vulkan_context->phys_dev == VK_NULL_HANDLE || vulkan_context->act_dev == VK_NULL_HANDLE) {
            throw Error(ErrorCode::processing_failed,
                        "FFmpeg returned an incomplete Vulkan device context");
        }

        VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &identity;
        vkGetPhysicalDeviceProperties2(vulkan_context->phys_dev, &properties);
        if (compact_uuid(identity.deviceUUID) != selected.uuid) {
            throw Error(ErrorCode::processing_failed,
                        "FFmpeg selected a different Vulkan device than requested");
        }

        for (int i = 0; i < vulkan_context->nb_qf; ++i) {
            const auto& family = vulkan_context->qf[i];
            if (family.num > 0 && (family.flags & VK_QUEUE_COMPUTE_BIT) != 0) {
                compute_family = static_cast<std::uint32_t>(family.idx);
                compute_count = static_cast<std::uint32_t>(family.num);
                break;
            }
        }
        if (compute_count == 0U) {
            throw Error(ErrorCode::unsupported_format,
                        "FFmpeg Vulkan device exposes no enabled compute queue");
        }
        for (int i = 0; i < vulkan_context->nb_enabled_inst_extensions; ++i) {
            instance_extensions.emplace_back(vulkan_context->enabled_inst_extensions[i]);
        }
        for (int i = 0; i < vulkan_context->nb_enabled_dev_extensions; ++i) {
            device_extensions.emplace_back(vulkan_context->enabled_dev_extensions[i]);
        }
    }

    AVHWDeviceContext* ffmpeg_device_context() const noexcept {
        return device_context
            ? reinterpret_cast<AVHWDeviceContext*>(device_context->data)
            : nullptr;
    }

    VulkanDeviceInfo selected;
    AvBufferRefPtr device_context;
    AVVulkanDeviceContext* vulkan_context{};
    std::uint32_t compute_family{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t compute_count{};
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
};

VulkanRuntime::VulkanRuntime(VulkanRuntimeConfig config)
    : impl_(std::make_unique<Impl>(config)) {}
VulkanRuntime::~VulkanRuntime() = default;
VulkanRuntime::VulkanRuntime(VulkanRuntime&&) noexcept = default;
VulkanRuntime& VulkanRuntime::operator=(VulkanRuntime&&) noexcept = default;
const VulkanDeviceInfo& VulkanRuntime::device() const noexcept { return impl_->selected; }
std::uint32_t VulkanRuntime::compute_queue_family() const noexcept { return impl_->compute_family; }
std::uint32_t VulkanRuntime::compute_queue_count() const noexcept { return impl_->compute_count; }
const std::vector<std::string>& VulkanRuntime::instance_extensions() const noexcept {
    return impl_->instance_extensions;
}
const std::vector<std::string>& VulkanRuntime::device_extensions() const noexcept {
    return impl_->device_extensions;
}
AVHWDeviceContext* VulkanRuntime::ffmpeg_device_context() const noexcept {
    return impl_->ffmpeg_device_context();
}
AVVulkanDeviceContext* VulkanRuntime::ffmpeg_vulkan_context() const noexcept {
    return impl_->vulkan_context;
}
AvBufferRefPtr VulkanRuntime::reference_device_context() const {
    auto* reference = av_buffer_ref(impl_->device_context.get());
    if (reference == nullptr) {
        throw Error(ErrorCode::processing_failed, "cannot reference FFmpeg Vulkan device context");
    }
    return AvBufferRefPtr(reference);
}

std::string_view vulkan_device_type_name(VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "other";
    default: return "unknown";
    }
}

} // namespace mcraw
