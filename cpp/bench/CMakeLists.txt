# Compiler identity and flags are baked into the binaries so every result
# prints the build it came from.
set(ORDERFLOW_BENCH_DEFS
  ORDERFLOW_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  ORDERFLOW_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")

add_executable(bench_replay bench_replay.cpp)
target_link_libraries(bench_replay PRIVATE orderflow orderflow_warnings)
target_compile_definitions(bench_replay PRIVATE ${ORDERFLOW_BENCH_DEFS})

add_executable(bench_structures bench_structures.cpp)
target_link_libraries(bench_structures PRIVATE orderflow orderflow_warnings benchmark::benchmark)

# The ring buffer, and the replay split into a feed thread and a book thread.
find_package(Threads REQUIRED)
add_executable(bench_spsc bench_spsc.cpp)
target_link_libraries(bench_spsc PRIVATE orderflow orderflow_warnings Threads::Threads)

add_executable(bench_pipeline bench_pipeline.cpp)
target_link_libraries(bench_pipeline PRIVATE orderflow orderflow_warnings Threads::Threads)
