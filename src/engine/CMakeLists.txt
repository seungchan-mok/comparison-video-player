find_package(Threads REQUIRED)

add_library(scvp_engine STATIC
    FfmpegCompat.h FfmpegCompat.cpp
    Frame.h
    FrameAllocator.h FrameAllocator.cpp
    FrameCache.h FrameCache.cpp
    DecodeLane.h DecodeLane.cpp
    Demux.h Demux.cpp
    Engine.h Engine.cpp
    FFmpegSource.h FFmpegSource.cpp
    FrameIndex.h FrameIndex.cpp
    IndexScanner.h IndexScanner.cpp
    Log.h Log.cpp
    Mailbox.h
    Platform.h Platform.cpp
    PrefetchPlanner.h PrefetchPlanner.cpp
    VideoSource.h
)
add_library(scvp::engine ALIAS scvp_engine)
target_include_directories(scvp_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(scvp_engine
    PUBLIC scvp::ffmpeg Threads::Threads
    PRIVATE scvp::compile_options)
