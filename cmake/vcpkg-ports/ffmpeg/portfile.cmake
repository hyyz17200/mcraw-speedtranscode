# The baseline FFmpeg port enables Vulkan but does not declare the SPIR-V
# compiler needed by prores_ks_vulkan. The overlay manifest adds the pinned
# host glslang tools; expose them while delegating the build recipe and patches
# to the exact port selected by the locked vcpkg baseline.
vcpkg_add_to_path("${CURRENT_HOST_INSTALLED_DIR}/tools/glslang")
set(CURRENT_PORT_DIR "${VCPKG_ROOT_DIR}/ports/ffmpeg")
set(_upstream_portfile "${CURRENT_PORT_DIR}/portfile.cmake")
file(READ "${_upstream_portfile}" _upstream_portfile_text)
string(REPLACE
  "        0050-fix-test-ld-absolute-lib-paths.patch"
  "        0050-fix-test-ld-absolute-lib-paths.patch\n        ${CMAKE_CURRENT_LIST_DIR}/prores-vulkan-async-depth.patch"
  _patched_portfile_text "${_upstream_portfile_text}")
set(_patched_portfile "${CURRENT_PORT_DIR}/mcraw-overlay-portfile.cmake")
file(WRITE "${_patched_portfile}" "${_patched_portfile_text}")
include("${_patched_portfile}")
