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
