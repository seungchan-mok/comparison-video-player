cmake_minimum_required(VERSION 3.21)

project(scvp
    VERSION 0.1.0
    DESCRIPTION "Simple comparison video player"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(SCVP_BUILD_TESTS "Build unit and integration tests" ON)
option(SCVP_BUILD_TOOLS "Build developer tools (scvp-bench)" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(ScvpCompilerFlags)
include(ScvpFFmpeg)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
qt_standard_project_setup()

add_subdirectory(src)
if(SCVP_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(SCVP_BUILD_TESTS)
    find_package(Qt6 6.4 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "Qt6::Test not found - tests disabled")
    endif()
endif()
