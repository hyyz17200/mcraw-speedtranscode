include(FindPackageHandleStandardArgs)

set(_FFMPEG_REQUIRED_COMPONENTS ${FFmpeg_FIND_COMPONENTS})
if(NOT _FFMPEG_REQUIRED_COMPONENTS)
  set(_FFMPEG_REQUIRED_COMPONENTS avcodec avformat avutil)
endif()

find_path(FFmpeg_INCLUDE_DIR libavcodec/avcodec.h)

foreach(_component IN LISTS _FFMPEG_REQUIRED_COMPONENTS)
  find_library(FFmpeg_${_component}_LIBRARY NAMES ${_component})
  if(FFmpeg_INCLUDE_DIR AND FFmpeg_${_component}_LIBRARY)
    set(FFmpeg_${_component}_FOUND TRUE)
    if(NOT TARGET FFmpeg::${_component})
      add_library(FFmpeg::${_component} UNKNOWN IMPORTED)
      set_target_properties(FFmpeg::${_component} PROPERTIES
        IMPORTED_LOCATION "${FFmpeg_${_component}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_DIR}"
      )
    endif()
  else()
    set(FFmpeg_${_component}_FOUND FALSE)
  endif()
endforeach()

find_package_handle_standard_args(
  FFmpeg
  REQUIRED_VARS FFmpeg_INCLUDE_DIR
  HANDLE_COMPONENTS
)

mark_as_advanced(FFmpeg_INCLUDE_DIR)
foreach(_component IN LISTS _FFMPEG_REQUIRED_COMPONENTS)
  mark_as_advanced(FFmpeg_${_component}_LIBRARY)
endforeach()
