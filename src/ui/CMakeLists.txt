add_library(scvp_ui STATIC
    EngineBridge.h EngineBridge.cpp
    FramePacer.h FramePacer.cpp
    KeyMap.h KeyMap.cpp
    KeyRouter.h KeyRouter.cpp
    MainWindow.h MainWindow.cpp
    Player.h Player.cpp
    VideoView.h VideoView.cpp
)
add_library(scvp::ui ALIAS scvp_ui)
target_include_directories(scvp_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(scvp_ui
    PUBLIC scvp::render Qt6::Widgets Qt6::OpenGLWidgets
    PRIVATE scvp::compile_options)
