add_library(scvp_render STATIC
    CodeReader.h CodeReader.cpp
    ColorParams.h ColorParams.cpp
    CudaInteropSource.h CudaInteropSource.cpp
    GlCommon.h
    PixelLayout.h PixelLayout.cpp
    CompareLayout.h CompareLayout.cpp
    Renderer.h Renderer.cpp
    RgbConverter.h RgbConverter.cpp
    ShaderLibrary.h ShaderLibrary.cpp
    TextureSource.h TextureSource.cpp
    ViewTransform.h ViewTransform.cpp
)
add_library(scvp::render ALIAS scvp_render)
target_include_directories(scvp_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(scvp_render
    PUBLIC scvp::engine Qt6::Gui Qt6::OpenGL
    PRIVATE scvp::compile_options)
qt_add_resources(scvp_render "scvp_shaders"
    PREFIX "/scvp"
    FILES
        shaders/fullscreen.vert
        shaders/convert.frag
        shaders/display.frag
)
