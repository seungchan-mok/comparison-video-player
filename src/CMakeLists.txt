add_subdirectory(engine)
add_subdirectory(render)
add_subdirectory(ui)

qt_add_executable(scvp WIN32 MACOSX_BUNDLE
    main.cpp
)
target_link_libraries(scvp PRIVATE
    scvp::compile_options
    scvp::ui
)
set_target_properties(scvp PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "SCVP"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.scvp.player"
)

if(WIN32)
    # Copy FFmpeg (and other imported) DLLs next to the executable so it runs from the build tree.
    add_custom_command(TARGET scvp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:scvp> $<TARGET_FILE_DIR:scvp>
        COMMAND_EXPAND_LISTS)
endif()
target_compile_definitions(scvp PRIVATE SCVP_VERSION="${PROJECT_VERSION}")
