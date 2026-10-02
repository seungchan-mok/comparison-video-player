scvp_add_test(tst_framepool tst_framepool.cpp)
scvp_add_test(tst_frameindex tst_frameindex.cpp)
scvp_add_test(tst_decodesession tst_decodesession.cpp
    FUNCTIONS sequential randomAccess randomAccessSkippingNonRef skippingNonRefDecodesFewerFrames
              backwardWalk cancellation)
scvp_add_test(tst_framecache tst_framecache.cpp)
scvp_add_test(tst_prefetchplanner tst_prefetchplanner.cpp)
scvp_add_test(tst_engine tst_engine.cpp
    FUNCTIONS showsRequestedFrame sequentialPlayback backwardStepping randomJumps
              shrinkingBudgetReleasesMemory twoVideosShareTheBudget closeWhileDecoding)
scvp_add_test(tst_playbackcontroller tst_playbackcontroller.cpp)
