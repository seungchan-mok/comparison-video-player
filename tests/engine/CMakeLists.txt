scvp_add_test(tst_framepool tst_framepool.cpp)
scvp_add_test(tst_frameindex tst_frameindex.cpp)
scvp_add_test(tst_decodesession tst_decodesession.cpp
    FUNCTIONS sequential randomAccess randomAccessSkippingNonRef skippingNonRefDecodesFewerFrames
              backwardWalk cancellation)
