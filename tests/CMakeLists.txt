# Smoke test: the executable starts headless and reports its library versions.
add_test(NAME smoke.version COMMAND scvp --version)
set_tests_properties(smoke.version PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    PASS_REGULAR_EXPRESSION "scvp ${PROJECT_VERSION}")
