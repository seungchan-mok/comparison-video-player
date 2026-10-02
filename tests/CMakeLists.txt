# Smoke test: the executable starts headless and reports its library versions.
add_test(NAME smoke.version COMMAND scvp --version)
set_tests_properties(smoke.version PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    PASS_REGULAR_EXPRESSION "scvp ${PROJECT_VERSION}")

# Generated test media (see tests/fixtures/gen_fixtures.sh). Tests that need it skip when absent.
set(SCVP_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/tests/fixtures/out" CACHE PATH "Directory with generated test media")
add_custom_target(fixtures
    COMMAND "${CMAKE_SOURCE_DIR}/tests/fixtures/gen_fixtures.sh" "${SCVP_FIXTURE_DIR}"
    USES_TERMINAL
    COMMENT "Generating test media into ${SCVP_FIXTURE_DIR}")

add_library(scvp_testsupport STATIC
    support/TestSupport.h support/TestSupport.cpp)
target_include_directories(scvp_testsupport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(scvp_testsupport PUBLIC scvp::engine Qt6::Core Qt6::Test PRIVATE scvp::compile_options)
target_compile_definitions(scvp_testsupport PRIVATE SCVP_FIXTURE_DIR="${SCVP_FIXTURE_DIR}")

add_library(scvp_gltestsupport STATIC support/GlTestContext.h support/GlTestContext.cpp)
target_link_libraries(scvp_gltestsupport PUBLIC scvp_testsupport scvp::render Qt6::Gui Qt6::OpenGL PRIVATE scvp::compile_options)

# scvp_add_test(<name> <sources...> [LIBS <targets...>] [FUNCTIONS <test functions...>])
# With FUNCTIONS, every listed QtTest function becomes its own ctest entry so `ctest -j` runs
# the slow fixture sweeps in parallel.
function(scvp_add_test name)
    cmake_parse_arguments(T "" "" "LIBS;FUNCTIONS" ${ARGN})
    add_executable(${name} ${T_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE scvp_testsupport scvp::compile_options ${T_LIBS})
    if(T_FUNCTIONS)
        foreach(fn IN LISTS T_FUNCTIONS)
            add_test(NAME ${name}.${fn} COMMAND ${name} ${fn})
            set_tests_properties(${name}.${fn} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endforeach()
    else()
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endfunction()

add_subdirectory(engine)
add_subdirectory(render)
add_subdirectory(e2e)
