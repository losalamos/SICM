add_library(nano OBJECT nano.c nano.h)
add_library(run_move OBJECT run_move.c run_move.h)
add_library(sizes OBJECT sizes.c sizes.h)

target_include_directories(run_move PRIVATE "${CMAKE_SOURCE_DIR}/include/low/public")

# simple executable for generating sizes
add_executable(generate_sizes generate_sizes.c sizes)

# example code
add_executable(move move.c sizes)
target_link_libraries(move PUBLIC sicm_SHARED)
target_link_libraries(move PRIVATE "${JEMALLOC_LDFLAGS}")

add_executable(move_perf move_perf.c nano run_move sizes)
target_link_libraries(move_perf PUBLIC sicm_SHARED)
target_link_libraries(move_perf PRIVATE "${JEMALLOC_LDFLAGS}")

configure_file(run_moves.sh run_moves.sh @ONLY)

# simple plotting script for loop_move_perf and bulk_move_perf
configure_file(plot_moves.sh plot_moves.sh @ONLY)

# C++ allocators
add_executable(allocators allocators.cpp)
target_link_libraries(allocators PUBLIC sicm_SHARED)
target_link_libraries(allocators PRIVATE "${JEMALLOC_LDFLAGS}")
