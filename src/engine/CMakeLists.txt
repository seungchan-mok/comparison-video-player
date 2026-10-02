find_package(Threads REQUIRED)

add_library(scvp_engine STATIC
    CudaApi.h CudaApi.cpp
    FfmpegCompat.h FfmpegCompat.cpp
    Frame.h
    FrameAllocator.h FrameAllocator.cpp
    FrameCache.h FrameCache.cpp
    DecodeLane.h DecodeLane.cpp
    Demux.h Demux.cpp
    Engine.h Engine.cpp
    FFmpegSource.h FFmpegSource.cpp
    FrameIndex.h FrameIndex.cpp
    GpuFrame.h GpuFrame.cpp
    HwDevice.h HwDevice.cpp
    IndexScanner.h IndexScanner.cpp
    Log.h Log.cpp
    Mailbox.h
    Platform.h Platform.cpp
    PlaybackController.h PlaybackController.cpp
    PrefetchPlanner.h PrefetchPlanner.cpp
    TimelineMapper.h TimelineMapper.cpp
    VideoSource.h
)
add_library(scvp::engine ALIAS scvp_engine)
target_include_directories(scvp_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(scvp_engine
    PUBLIC scvp::ffmpeg Threads::Threads ${CMAKE_DL_LIBS}
    PRIVATE scvp::compile_options)
