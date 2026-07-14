cmake_minimum_required(VERSION 3.25)

project(mcraw_transcoder VERSION 0.1.0 LANGUAGES CXX)

option(MCRAW_BUILD_TESTS "Build unit and sample tests" ON)
option(MCRAW_FETCH_DEPENDENCIES "Fetch MotionCam, librtprocess, and Catch2" ON)
option(MCRAW_ENABLE_FFMPEG "Build ProRes/MOV output support" ON)
option(MCRAW_ENABLE_RTPROCESS "Build AMaZE/RCD/IGV support" ON)

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
  find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swresample swscale)
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

if(MCRAW_ENABLE_FFMPEG)
  target_sources(mcraw_core PRIVATE src/output/ffmpeg_writer.cpp)
  target_link_libraries(mcraw_core PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swresample FFmpeg::swscale)
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
  )
  target_link_libraries(mcraw_tests PRIVATE mcraw_core Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(mcraw_tests)
endif()
