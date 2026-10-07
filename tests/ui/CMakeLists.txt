scvp_add_test(tst_keyrouter tst_keyrouter.cpp LIBS scvp::ui)
scvp_add_test(tst_timeline tst_timeline.cpp LIBS scvp::ui)
scvp_add_test(tst_stage tst_stage.cpp LIBS scvp::ui)
scvp_add_test(tst_settings tst_settings.cpp LIBS scvp::ui)
scvp_add_test(tst_openrawdialog tst_openrawdialog.cpp LIBS scvp::ui)
scvp_add_test(tst_legenddialog tst_legenddialog.cpp LIBS scvp::ui)
scvp_add_test(tst_clipboardimage tst_clipboardimage.cpp LIBS scvp::ui)
scvp_add_test(tst_framepacer tst_framepacer.cpp LIBS scvp::ui)
scvp_add_test(tst_commandline tst_commandline.cpp LIBS scvp::ui)
if(UNIX AND NOT APPLE)
    scvp_add_test(tst_desktopintegration tst_desktopintegration.cpp LIBS scvp::ui)
    target_compile_definitions(tst_desktopintegration PRIVATE SCVP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
scvp_add_test(tst_overlaypainter tst_overlaypainter.cpp LIBS scvp::ui)
scvp_add_test(tst_exportjob tst_exportjob.cpp LIBS scvp::ui scvp_gltestsupport
    FUNCTIONS alignedDifferenceIsBlack splitIsTheRenderedPair overlayStaysInItsCorners rangeAsPngSequence
              cancelLeavesNothing drawingOnTheGuiThread encoderErrorsAreReported)
scvp_add_test(tst_exportdialog tst_exportdialog.cpp LIBS scvp::ui)

# Export throughput on the current GPU and encoders (not a ctest; run by hand):
#   QT_QPA_PLATFORM=offscreen build/dev/tests/ui/bench_export a.mkv b.mkv --format hevc --frames 120
add_executable(bench_export bench_export.cpp)
target_link_libraries(bench_export PRIVATE scvp::ui Qt6::Test scvp::compile_options)
