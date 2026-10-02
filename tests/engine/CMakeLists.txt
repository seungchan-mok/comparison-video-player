scvp_add_test(tst_framepool tst_framepool.cpp)
scvp_add_test(tst_frameindex tst_frameindex.cpp)
scvp_add_test(tst_decodesession tst_decodesession.cpp
    FUNCTIONS sequential randomAccess randomAccessSkippingNonRef skippingNonRefDecodesFewerFrames
              backwardWalk cancellation)
scvp_add_test(tst_framecache tst_framecache.cpp)
scvp_add_test(tst_prefetchplanner tst_prefetchplanner.cpp)
scvp_add_test(tst_engine tst_engine.cpp
    FUNCTIONS showsRequestedFrame sequentialPlayback playingToTheEndDecodesEachFrameOnce backwardStepping randomJumps
              shrinkingBudgetReleasesMemory twoVideosShareTheBudget gpuTier jumpTargetStaysOnGpu closeWhileDecoding)
scvp_add_test(tst_rawvideo tst_rawvideo.cpp)
scvp_add_test(tst_imagesequence tst_imagesequence.cpp)
scvp_add_test(tst_timelinemapper tst_timelinemapper.cpp)
scvp_add_test(tst_playbackcontroller tst_playbackcontroller.cpp)
scvp_add_test(tst_hwdecode tst_hwdecode.cpp FUNCTIONS sequential randomAccess fallbackToSoftware)
scvp_add_test(tst_gpuframes tst_gpuframes.cpp)
