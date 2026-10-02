# Warning and optimization flags shared by all scvp targets.
add_library(scvp_compile_options INTERFACE)
add_library(scvp::compile_options ALIAS scvp_compile_options)

if(MSVC)
    target_compile_options(scvp_compile_options INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    target_compile_definitions(scvp_compile_options INTERFACE NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(scvp_compile_options INTERFACE -Wall -Wextra)
    # Keep frame pointers in optimized builds with debug info so perf/gdb stacks stay usable.
    target_compile_options(scvp_compile_options INTERFACE
        $<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>)
endif()

option(SCVP_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(SCVP_TSAN "Build with ThreadSanitizer" OFF)
if(SCVP_SANITIZE AND NOT MSVC)
    target_compile_options(scvp_compile_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(scvp_compile_options INTERFACE -fsanitize=address,undefined)
endif()
if(SCVP_TSAN AND NOT MSVC)
    target_compile_options(scvp_compile_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(scvp_compile_options INTERFACE -fsanitize=thread)
endif()
