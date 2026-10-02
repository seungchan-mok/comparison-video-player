add_executable(scvp-bench scvp-bench.cpp)
target_link_libraries(scvp-bench PRIVATE scvp::engine scvp::compile_options)
