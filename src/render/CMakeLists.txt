add_library(scvp_render STATIC
    ColorParams.h ColorParams.cpp
    PixelLayout.h PixelLayout.cpp
)
add_library(scvp::render ALIAS scvp_render)
target_include_directories(scvp_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(scvp_render
    PUBLIC scvp::engine Qt6::Gui Qt6::OpenGL
    PRIVATE scvp::compile_options)
