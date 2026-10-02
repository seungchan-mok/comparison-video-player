# End-to-end: the real application on a private Xvfb display (scripts/xvfb-run.sh).
find_program(SCVP_BASH bash)
if(UNIX AND NOT APPLE AND SCVP_BASH)
    scvp_add_test(tst_app_grab tst_app_grab.cpp LIBS scvp::render)
    target_compile_definitions(tst_app_grab PRIVATE
        SCVP_APP_PATH="$<TARGET_FILE:scvp>"
        SCVP_XVFB_RUN="${CMAKE_SOURCE_DIR}/scripts/xvfb-run.sh")
    add_dependencies(tst_app_grab scvp)
endif()
