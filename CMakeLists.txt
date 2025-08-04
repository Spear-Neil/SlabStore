cmake_minimum_required(VERSION 3.10)
project(SlabStore)
set(CMAKE_CXX_STANDARD 17)

include_directories(util)

execute_process(COMMAND lscpu COMMAND grep avx2 OUTPUT_VARIABLE AVX2_ENABLE)
if (NOT AVX2_ENABLE)
    message(SEND_ERROR "avx2 is not supported")
endif ()

link_libraries(pthread numa jemalloc tbb)
add_compile_options(-march=native -fopt-info-vec-optimized)

add_subdirectory(unit)
add_executable(SlabStore main.cpp)