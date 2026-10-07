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

if(WIN32 AND SCVP_FFMPEG_RUNTIME_DLLS)
    # Copy the FFmpeg DLLs next to the executable so it runs from the build tree.
    add_custom_command(TARGET scvp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SCVP_FFMPEG_RUNTIME_DLLS} $<TARGET_FILE_DIR:scvp>)
endif()
target_compile_definitions(scvp PRIVATE SCVP_VERSION="${PROJECT_VERSION}")

# Installation and packages (cpack): Qt's deploy script bundles Qt on Windows and macOS. On
# Windows the FFmpeg DLLs and the VC++ runtime go next to the executable, so the package runs on
# a PC with nothing else installed (scripts/check-windows-bundle.ps1 verifies it in CI).
include(GNUInstallDirs)
install(TARGETS scvp
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32 OR APPLE)
    set(scvp_deploy_options)
    if(WIN32 AND Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
        # No vc_redist.x64.exe for the user to run (the runtime DLLs are installed below), and no
        # D3D12 shader compiler: the program draws with OpenGL.
        set(scvp_deploy_options NO_COMPILER_RUNTIME DEPLOY_TOOL_OPTIONS --no-system-dxc-compiler)
    endif()
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.5)
        qt_generate_deploy_app_script(TARGET scvp OUTPUT_SCRIPT scvp_deploy NO_UNSUPPORTED_PLATFORM_ERROR
            ${scvp_deploy_options})
    else()
        qt_generate_deploy_app_script(TARGET scvp FILENAME_VARIABLE scvp_deploy NO_UNSUPPORTED_PLATFORM_ERROR)
    endif()
    install(SCRIPT ${scvp_deploy})
endif()
if(WIN32)
    install(FILES ${SCVP_FFMPEG_RUNTIME_DLLS} DESTINATION ${CMAKE_INSTALL_BINDIR})
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    include(InstallRequiredSystemLibraries)
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
