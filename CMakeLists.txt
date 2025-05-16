cmake_minimum_required(VERSION 3.10)
project(SlabStore)
set(CMAKE_CXX_STANDARD 17)

include_directories(util)

link_libraries(pthread numa jemalloc tbb)
add_compile_options(-march=native -fopt-info-vec-optimized)

add_subdirectory(unit)
add_executable(SlabStore main.cpp)