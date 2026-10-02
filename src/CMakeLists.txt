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
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/Info.plist.in"
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
    # Applications-menu entry, file types and icons (the icons are drawn by the program itself).
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/scvp.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/scvp-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages
            RENAME scvp.xml)
    install(CODE "execute_process(COMMAND \"$<TARGET_FILE:scvp>\" --export-icons
                      \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor\"
                  RESULT_VARIABLE scvp_icons_result)
                  if(NOT scvp_icons_result EQUAL 0)
                      message(FATAL_ERROR \"exporting the application icons failed\")
                  endif()")
endif()
