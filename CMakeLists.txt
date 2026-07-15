cmake_minimum_required(VERSION 3.25)

project(mcraw_transcoder VERSION 0.1.0 LANGUAGES CXX)

option(MCRAW_BUILD_TESTS "Build unit and sample tests" ON)
option(MCRAW_FETCH_DEPENDENCIES "Fetch MotionCam, librtprocess, and Catch2" ON)
option(MCRAW_ENABLE_FFMPEG "Build ProRes/MOV output support" ON)
option(MCRAW_ENABLE_VULKAN "Build the optional Vulkan video backend" ON)
option(MCRAW_ENABLE_RTPROCESS "Build AMaZE/RCD/IGV support" ON)
option(MCRAW_ENABLE_AVX2 "Build CPU processing targets for AVX2-capable processors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
find_package(OpenMP REQUIRED COMPONENTS CXX)

if(MCRAW_FETCH_DEPENDENCIES)
  # The official decoder is the bit-exact CPU truth source for compression 6/7.
  FetchContent_Declare(
    motioncam_decoder_source
    GIT_REPOSITORY https://github.com/mirsadm/motioncam-decoder.git
    GIT_TAG 06bf1a8 # immutable commit behind release/0.2
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(motioncam_decoder_source)

  if(MCRAW_ENABLE_RTPROCESS)
    FetchContent_Declare(
      rtprocess_source
      GIT_REPOSITORY https://github.com/CarVac/librtprocess.git
      GIT_TAG 0.12.0
      GIT_SHALLOW TRUE
      # Populate the pinned source without building unrelated demosaic,
      # X-Trans, CA-correction, and highlight-recovery algorithms.
      SOURCE_SUBDIR mcraw-unused-cmake-subdir
    )
    FetchContent_MakeAvailable(rtprocess_source)

    add_library(rtprocess STATIC
      "${rtprocess_source_SOURCE_DIR}/src/demosaic/amaze.cc"
      "${rtprocess_source_SOURCE_DIR}/src/demosaic/border.cc"
      "${rtprocess_source_SOURCE_DIR}/src/demosaic/dcb.cc"
      "${rtprocess_source_SOURCE_DIR}/src/demosaic/igv.cc"
      "${rtprocess_source_SOURCE_DIR}/src/demosaic/lmmse.cc"
      "${rtprocess_source_SOURCE_DIR}/src/demosaic/rcd.cc"
    )
    add_library(rtprocess::rtprocess ALIAS rtprocess)
    target_include_directories(rtprocess PUBLIC
      "$<BUILD_INTERFACE:${rtprocess_source_SOURCE_DIR}/src/include>"
    )
    target_link_libraries(rtprocess PRIVATE OpenMP::OpenMP_CXX)
    if(WIN32)
      target_compile_definitions(rtprocess PRIVATE NOMINMAX)
    endif()
  endif()
endif()

if(NOT TARGET motioncam_decoder)
  message(FATAL_ERROR "motioncam_decoder target is required")
endif()

if(MCRAW_ENABLE_FFMPEG)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil)
endif()

if(MCRAW_ENABLE_VULKAN)
  if(NOT MCRAW_ENABLE_FFMPEG)
    message(FATAL_ERROR "MCRAW_ENABLE_VULKAN requires MCRAW_ENABLE_FFMPEG")
  endif()
  find_package(VulkanHeaders CONFIG REQUIRED)
  find_package(VulkanLoader CONFIG REQUIRED)
endif()

add_library(mcraw_core
  src/core/config.cpp
  src/core/metadata.cpp
  src/core/pixel_types.cpp
  src/core/timing.cpp
  src/io/mcraw_reader.cpp
  src/processing/calibration.cpp
  src/processing/color.cpp
  src/processing/demosaic.cpp
  src/processing/log_curve.cpp
  src/processing/pipeline.cpp
  src/processing/yuv.cpp
  src/output/sidecar.cpp
  src/output/backend_selection.cpp
)

target_include_directories(mcraw_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${motioncam_decoder_source_SOURCE_DIR}/thirdparty>"
  PRIVATE
    "${motioncam_decoder_source_SOURCE_DIR}/lib/include"
)

target_link_libraries(mcraw_core
  PRIVATE motioncam_decoder OpenMP::OpenMP_CXX
)

target_compile_definitions(mcraw_core PUBLIC
  MCRAW_HAS_RTPROCESS=$<BOOL:${MCRAW_ENABLE_RTPROCESS}>
  MCRAW_HAS_FFMPEG=$<BOOL:${MCRAW_ENABLE_FFMPEG}>
  MCRAW_HAS_VULKAN=$<BOOL:${MCRAW_ENABLE_VULKAN}>
)

if(MCRAW_ENABLE_RTPROCESS)
  if(TARGET rtprocess)
    target_link_libraries(mcraw_core PRIVATE rtprocess)
  elseif(TARGET rtprocess::rtprocess)
    target_link_libraries(mcraw_core PRIVATE rtprocess::rtprocess)
  else()
    message(FATAL_ERROR "librtprocess was fetched but exposed no known CMake target")
  endif()
  target_include_directories(mcraw_core PRIVATE "${rtprocess_source_SOURCE_DIR}/src/include")
endif()

if(MSVC)
  target_compile_options(mcraw_core PRIVATE /W4 /permissive- /utf-8 /EHsc /fp:strict /Zc:preprocessor)
  target_compile_definitions(mcraw_core PUBLIC _WIN32_WINNT=0x0A00)
else()
  target_compile_options(mcraw_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

# The analytic reference path keeps /fp:strict for reproducible truth values.
# The production per-pixel loops are tolerance-validated against that path
# (unit tests and validate --compare-fused), so they may trade strict IEEE
# ordering for vectorization and FMA contraction.
set(MCRAW_FAST_MATH_SOURCES
  src/processing/calibration.cpp
  src/processing/demosaic.cpp
  src/processing/yuv.cpp
)
if(MSVC)
  set_source_files_properties(${MCRAW_FAST_MATH_SOURCES} PROPERTIES
    COMPILE_OPTIONS "/fp:fast")
else()
  # -fno-finite-math-only keeps the pipeline's isfinite() input guards valid.
  set_source_files_properties(${MCRAW_FAST_MATH_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-ffast-math;-fno-finite-math-only")
endif()

if(MCRAW_ENABLE_AVX2)
  if(MSVC)
    set(MCRAW_AVX2_COMPILE_OPTION /arch:AVX2)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(MCRAW_AVX2_COMPILE_OPTION -mavx2)
  else()
    message(FATAL_ERROR "MCRAW_ENABLE_AVX2 is unsupported by ${CMAKE_CXX_COMPILER_ID}")
  endif()

  target_compile_options(mcraw_core PRIVATE ${MCRAW_AVX2_COMPILE_OPTION})
  target_compile_options(motioncam_decoder PRIVATE ${MCRAW_AVX2_COMPILE_OPTION})
  if(TARGET rtprocess)
    target_compile_options(rtprocess PRIVATE ${MCRAW_AVX2_COMPILE_OPTION})
  endif()
endif()

if(MCRAW_ENABLE_FFMPEG)
  target_sources(mcraw_core PRIVATE
    src/output/cpu_prores_encoder.cpp
    src/output/ffmpeg_raii.cpp
    src/output/ffmpeg_writer.cpp
    src/output/vulkan_prores_encoder_stub.cpp
  )
  target_link_libraries(mcraw_core
    PUBLIC FFmpeg::avcodec FFmpeg::avutil
    PRIVATE FFmpeg::avformat
  )
endif()

if(MCRAW_ENABLE_VULKAN)
  find_program(MCRAW_GLSLANG_VALIDATOR
    NAMES glslangValidator
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/glslang"
    REQUIRED)
  set(MCRAW_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/mcraw/vulkan")
  set(MCRAW_RGB_TO_YUV_SPV "${MCRAW_GENERATED_SHADER_DIR}/rgb_to_yuv_422.comp.spv")
  set(MCRAW_RGB_TO_YUV_HEADER "${MCRAW_GENERATED_SHADER_DIR}/rgb_to_yuv_422_spv.hpp")
  set(MCRAW_RGB_TO_YUV_IMAGE_SPV "${MCRAW_GENERATED_SHADER_DIR}/rgb_to_yuv_422_image.comp.spv")
  set(MCRAW_RGB_TO_YUV_IMAGE_HEADER "${MCRAW_GENERATED_SHADER_DIR}/rgb_to_yuv_422_image_spv.hpp")
  set(MCRAW_CAMERA_TO_DWG_SPV "${MCRAW_GENERATED_SHADER_DIR}/camera_to_dwg.comp.spv")
  set(MCRAW_CAMERA_TO_DWG_HEADER "${MCRAW_GENERATED_SHADER_DIR}/camera_to_dwg_spv.hpp")
  set(MCRAW_SHARPEN_TARGET_LINEAR_SPV "${MCRAW_GENERATED_SHADER_DIR}/sharpen_target_linear.comp.spv")
  set(MCRAW_SHARPEN_TARGET_LINEAR_HEADER "${MCRAW_GENERATED_SHADER_DIR}/sharpen_target_linear_spv.hpp")
  set(MCRAW_DAVINCI_INTERMEDIATE_SPV "${MCRAW_GENERATED_SHADER_DIR}/davinci_intermediate.comp.spv")
  set(MCRAW_DAVINCI_INTERMEDIATE_HEADER "${MCRAW_GENERATED_SHADER_DIR}/davinci_intermediate_spv.hpp")
  set(MCRAW_CALIBRATE_RAW_SPV "${MCRAW_GENERATED_SHADER_DIR}/calibrate_raw.comp.spv")
  set(MCRAW_CALIBRATE_RAW_HEADER "${MCRAW_GENERATED_SHADER_DIR}/calibrate_raw_spv.hpp")
  set(MCRAW_RCD_DEMOSAIC_SPV "${MCRAW_GENERATED_SHADER_DIR}/rcd_demosaic.comp.spv")
  set(MCRAW_RCD_DEMOSAIC_HEADER "${MCRAW_GENERATED_SHADER_DIR}/rcd_demosaic_spv.hpp")
  add_custom_command(
    OUTPUT "${MCRAW_RCD_DEMOSAIC_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MCRAW_GENERATED_SHADER_DIR}"
    COMMAND "${MCRAW_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
            -o "${MCRAW_RCD_DEMOSAIC_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/rcd_demosaic.comp.glsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/rcd_demosaic.comp.glsl"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_RCD_DEMOSAIC_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${MCRAW_RCD_DEMOSAIC_SPV}"
            "-DOUTPUT=${MCRAW_RCD_DEMOSAIC_HEADER}"
            "-DSYMBOL=rcd_demosaic_spv"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    DEPENDS "${MCRAW_RCD_DEMOSAIC_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_CALIBRATE_RAW_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MCRAW_GENERATED_SHADER_DIR}"
    COMMAND "${MCRAW_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
            -o "${MCRAW_CALIBRATE_RAW_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/calibrate_raw.comp.glsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/calibrate_raw.comp.glsl"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_CALIBRATE_RAW_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${MCRAW_CALIBRATE_RAW_SPV}"
            "-DOUTPUT=${MCRAW_CALIBRATE_RAW_HEADER}"
            "-DSYMBOL=calibrate_raw_spv"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    DEPENDS "${MCRAW_CALIBRATE_RAW_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_RGB_TO_YUV_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MCRAW_GENERATED_SHADER_DIR}"
    COMMAND "${MCRAW_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
            -o "${MCRAW_RGB_TO_YUV_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/rgb_to_yuv_422.comp.glsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/rgb_to_yuv_422.comp.glsl"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_RGB_TO_YUV_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${MCRAW_RGB_TO_YUV_SPV}"
            "-DOUTPUT=${MCRAW_RGB_TO_YUV_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    DEPENDS "${MCRAW_RGB_TO_YUV_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_RGB_TO_YUV_IMAGE_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MCRAW_GENERATED_SHADER_DIR}"
    COMMAND "${MCRAW_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
            -o "${MCRAW_RGB_TO_YUV_IMAGE_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/rgb_to_yuv_422_image.comp.glsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/rgb_to_yuv_422_image.comp.glsl"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_RGB_TO_YUV_IMAGE_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${MCRAW_RGB_TO_YUV_IMAGE_SPV}"
            "-DOUTPUT=${MCRAW_RGB_TO_YUV_IMAGE_HEADER}"
            "-DSYMBOL=rgb_to_yuv_422_image_spv"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    DEPENDS "${MCRAW_RGB_TO_YUV_IMAGE_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_CAMERA_TO_DWG_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MCRAW_GENERATED_SHADER_DIR}"
    COMMAND "${MCRAW_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
            -o "${MCRAW_CAMERA_TO_DWG_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/camera_to_dwg.comp.glsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/camera_to_dwg.comp.glsl"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_CAMERA_TO_DWG_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${MCRAW_CAMERA_TO_DWG_SPV}"
            "-DOUTPUT=${MCRAW_CAMERA_TO_DWG_HEADER}"
            "-DSYMBOL=camera_to_dwg_spv"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    DEPENDS "${MCRAW_CAMERA_TO_DWG_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_SHARPEN_TARGET_LINEAR_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MCRAW_GENERATED_SHADER_DIR}"
    COMMAND "${MCRAW_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
            -o "${MCRAW_SHARPEN_TARGET_LINEAR_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/sharpen_target_linear.comp.glsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/sharpen_target_linear.comp.glsl"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_SHARPEN_TARGET_LINEAR_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${MCRAW_SHARPEN_TARGET_LINEAR_SPV}"
            "-DOUTPUT=${MCRAW_SHARPEN_TARGET_LINEAR_HEADER}"
            "-DSYMBOL=sharpen_target_linear_spv"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    DEPENDS "${MCRAW_SHARPEN_TARGET_LINEAR_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_DAVINCI_INTERMEDIATE_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MCRAW_GENERATED_SHADER_DIR}"
    COMMAND "${MCRAW_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
            -o "${MCRAW_DAVINCI_INTERMEDIATE_SPV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/davinci_intermediate.comp.glsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/shaders/davinci_intermediate.comp.glsl"
    VERBATIM)
  add_custom_command(
    OUTPUT "${MCRAW_DAVINCI_INTERMEDIATE_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${MCRAW_DAVINCI_INTERMEDIATE_SPV}"
            "-DOUTPUT=${MCRAW_DAVINCI_INTERMEDIATE_HEADER}"
            "-DSYMBOL=davinci_intermediate_spv"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    DEPENDS "${MCRAW_DAVINCI_INTERMEDIATE_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
    VERBATIM)
  function(mcraw_compile_embedded_shader name source symbol)
    set(spv "${MCRAW_GENERATED_SHADER_DIR}/${name}.comp.spv")
    set(header "${MCRAW_GENERATED_SHADER_DIR}/${name}_spv.hpp")
    add_custom_command(
      OUTPUT "${spv}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${MCRAW_GENERATED_SHADER_DIR}"
      COMMAND "${MCRAW_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
              -o "${spv}" "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
      VERBATIM)
    add_custom_command(
      OUTPUT "${header}"
      COMMAND "${CMAKE_COMMAND}" "-DINPUT=${spv}" "-DOUTPUT=${header}"
              "-DSYMBOL=${symbol}"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
      DEPENDS "${spv}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-spirv.cmake"
      VERBATIM)
    set(${name}_HEADER "${header}" PARENT_SCOPE)
  endfunction()
  mcraw_compile_embedded_shader(camera_to_dwg_fp16_storage
    src/vulkan/shaders/camera_to_dwg_fp16_storage.comp.glsl
    camera_to_dwg_fp16_storage_spv)
  mcraw_compile_embedded_shader(sharpen_target_linear_fp16_storage
    src/vulkan/shaders/sharpen_target_linear_fp16_storage.comp.glsl
    sharpen_target_linear_fp16_storage_spv)
  mcraw_compile_embedded_shader(davinci_intermediate_fp16_storage
    src/vulkan/shaders/davinci_intermediate_fp16_storage.comp.glsl
    davinci_intermediate_fp16_storage_spv)
  mcraw_compile_embedded_shader(davinci_intermediate_fp16_analytic
    src/vulkan/shaders/davinci_intermediate_fp16_analytic.comp.glsl
    davinci_intermediate_fp16_analytic_spv)
  mcraw_compile_embedded_shader(rgb_to_yuv_422_image_fp16_storage
    src/vulkan/shaders/rgb_to_yuv_422_image_fp16_storage.comp.glsl
    rgb_to_yuv_422_image_fp16_storage_spv)
  add_custom_target(mcraw_vulkan_shaders DEPENDS
    "${MCRAW_RGB_TO_YUV_HEADER}" "${MCRAW_RGB_TO_YUV_IMAGE_HEADER}"
    "${MCRAW_CAMERA_TO_DWG_HEADER}" "${MCRAW_SHARPEN_TARGET_LINEAR_HEADER}"
    "${MCRAW_DAVINCI_INTERMEDIATE_HEADER}" "${MCRAW_CALIBRATE_RAW_HEADER}"
    "${MCRAW_RCD_DEMOSAIC_HEADER}"
    "${camera_to_dwg_fp16_storage_HEADER}"
    "${sharpen_target_linear_fp16_storage_HEADER}"
    "${davinci_intermediate_fp16_storage_HEADER}"
    "${davinci_intermediate_fp16_analytic_HEADER}"
    "${rgb_to_yuv_422_image_fp16_storage_HEADER}")
  add_dependencies(mcraw_core mcraw_vulkan_shaders)
  target_include_directories(mcraw_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_sources(mcraw_core PRIVATE
    src/output/ffmpeg_vulkan_context.cpp
    src/output/vulkan_prores_encoder.cpp
    src/vulkan/vulkan_rgb_to_yuv.cpp
    src/vulkan/vulkan_rgb_to_yuv_frame.cpp
    src/vulkan/vulkan_camera_pipeline_resources.cpp
    src/vulkan/vulkan_raw_pipeline_resources.cpp
    src/vulkan/vulkan_runtime.cpp
  )
  target_link_libraries(mcraw_core PUBLIC Vulkan::Headers Vulkan::Loader)
endif()

add_executable(mcraw-transcoder src/cli/main.cpp)
target_link_libraries(mcraw-transcoder PRIVATE mcraw_core)

if(MCRAW_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")

  add_executable(mcraw_tests
    tests/test_calibration.cpp
    tests/test_color.cpp
    tests/test_demosaic.cpp
    tests/test_log_curve.cpp
    tests/test_metadata.cpp
    tests/test_timing.cpp
    tests/test_yuv.cpp
    tests/test_backend_selection.cpp
  )
  if(MCRAW_ENABLE_FFMPEG)
    target_sources(mcraw_tests PRIVATE tests/test_video_encoder.cpp)
  endif()
  if(MCRAW_ENABLE_VULKAN)
    target_sources(mcraw_tests PRIVATE
      tests/test_ffmpeg_vulkan_context.cpp
      tests/test_vulkan_prores_e2e.cpp
      tests/test_vulkan_prores_encoder.cpp
      tests/test_vulkan_camera_pipeline_resources.cpp
      tests/test_vulkan_raw_pipeline_resources.cpp
      tests/test_vulkan_rgb_to_yuv.cpp
      tests/test_vulkan_runtime.cpp
    )
  endif()
  target_link_libraries(mcraw_tests PRIVATE mcraw_core Catch2::Catch2WithMain)
  if(MCRAW_ENABLE_VULKAN)
    target_link_libraries(mcraw_tests PRIVATE FFmpeg::avformat)
    if(WIN32)
      target_link_libraries(mcraw_tests PRIVATE Psapi)
    endif()
  endif()
  include(Catch)
  catch_discover_tests(mcraw_tests)
endif()
