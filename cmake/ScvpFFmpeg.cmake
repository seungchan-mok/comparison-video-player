# Locates FFmpeg (libavformat, libavcodec, libavutil, libswscale) and exposes them as
# imported targets FFmpeg::<component> plus the interface target scvp::ffmpeg.
#
#  * Default: pkg-config (Linux distributions, Homebrew).
#  * FFMPEG_ROOT=<prefix> with include/ and lib/ (and bin/ on Windows): prebuilt SDKs such as
#    the BtbN Windows builds, or the no-root Ubuntu prefix made by scripts/fetch-ubuntu-deps.sh.
#
# Minimum supported version: FFmpeg 6.1 (libavcodec 60.31, libavformat 60.16, libavutil 58.29).

set(SCVP_FFMPEG_COMPONENTS avformat avcodec avutil swscale)
set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}" CACHE PATH "FFmpeg prefix with include/ and lib/ (empty: use pkg-config)")

add_library(scvp_ffmpeg INTERFACE)
add_library(scvp::ffmpeg ALIAS scvp_ffmpeg)

if(FFMPEG_ROOT)
    find_path(SCVP_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
        PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    foreach(comp IN LISTS SCVP_FFMPEG_COMPONENTS)
        find_library(SCVP_FFMPEG_${comp}_LIBRARY NAMES ${comp}
            PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
        if(WIN32)
            file(GLOB _scvp_dll "${FFMPEG_ROOT}/bin/${comp}-*.dll")
            if(NOT _scvp_dll)
                message(FATAL_ERROR "FFmpeg DLL for ${comp} not found in ${FFMPEG_ROOT}/bin")
            endif()
            list(GET _scvp_dll 0 _scvp_dll)
            add_library(FFmpeg::${comp} SHARED IMPORTED)
            set_target_properties(FFmpeg::${comp} PROPERTIES
                IMPORTED_IMPLIB "${SCVP_FFMPEG_${comp}_LIBRARY}"
                IMPORTED_LOCATION "${_scvp_dll}")
        else()
            add_library(FFmpeg::${comp} UNKNOWN IMPORTED)
            set_target_properties(FFmpeg::${comp} PROPERTIES
                IMPORTED_LOCATION "${SCVP_FFMPEG_${comp}_LIBRARY}")
        endif()
        set_target_properties(FFmpeg::${comp} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SCVP_FFMPEG_INCLUDE_DIR}")
        target_link_libraries(scvp_ffmpeg INTERFACE FFmpeg::${comp})
    endforeach()
    message(STATUS "FFmpeg: using FFMPEG_ROOT=${FFMPEG_ROOT}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SCVP_LIBAV REQUIRED IMPORTED_TARGET
        libavformat>=60.16 libavcodec>=60.31 libavutil>=58.29 libswscale>=7.5)
    target_link_libraries(scvp_ffmpeg INTERFACE PkgConfig::SCVP_LIBAV)
    message(STATUS "FFmpeg: libavcodec ${SCVP_LIBAV_libavcodec_VERSION} (pkg-config)")
endif()
