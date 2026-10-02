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

# Installation and packages (cpack): Qt's deploy script bundles Qt on Windows and macOS; on
# Windows the FFmpeg DLLs are installed next to the executable.
include(GNUInstallDirs)
install(TARGETS scvp
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32 OR APPLE)
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.5)
        qt_generate_deploy_app_script(TARGET scvp OUTPUT_SCRIPT scvp_deploy NO_UNSUPPORTED_PLATFORM_ERROR)
    else()
        qt_generate_deploy_app_script(TARGET scvp FILENAME_VARIABLE scvp_deploy NO_UNSUPPORTED_PLATFORM_ERROR)
    endif()
    install(SCRIPT ${scvp_deploy})
endif()
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:scvp> DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(UNIX AND NOT APPLE)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/scvp.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()
