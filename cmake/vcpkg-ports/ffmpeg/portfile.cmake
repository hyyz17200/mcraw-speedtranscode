# The baseline FFmpeg port enables Vulkan but does not declare the SPIR-V
# compiler needed by prores_ks_vulkan. The overlay manifest adds the pinned
# host glslang tools; expose them while delegating the build recipe and patches
# to the exact port selected by the locked vcpkg baseline.
vcpkg_add_to_path("${CURRENT_HOST_INSTALLED_DIR}/tools/glslang")
set(CURRENT_PORT_DIR "${VCPKG_ROOT_DIR}/ports/ffmpeg")
include("${CURRENT_PORT_DIR}/portfile.cmake")
