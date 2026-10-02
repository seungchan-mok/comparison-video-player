scvp_add_test(tst_pixellayout tst_pixellayout.cpp LIBS scvp::render)
scvp_add_test(tst_rgbconverter tst_rgbconverter.cpp LIBS scvp_gltestsupport)
if(UNIX AND NOT APPLE)
    # Same GL tests on Mesa's software rasterizer: a second driver with different rounding.
    add_test(NAME tst_rgbconverter.mesa COMMAND tst_rgbconverter)
    set_tests_properties(tst_rgbconverter.mesa PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;__GLX_VENDOR_LIBRARY_NAME=mesa;LIBGL_ALWAYS_SOFTWARE=1")
endif()
scvp_add_test(tst_viewtransform tst_viewtransform.cpp LIBS scvp::render)
scvp_add_test(tst_display tst_display.cpp LIBS scvp_gltestsupport)
