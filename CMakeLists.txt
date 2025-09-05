cmake_minimum_required(VERSION 3.10)
project(SlabStore)
set(CMAKE_CXX_STANDARD 17)

include_directories(util)

execute_process(COMMAND lscpu COMMAND grep sse2 OUTPUT_VARIABLE SSE2_ENABLE)
if (NOT SSE2_ENABLE)
    message(SEND_ERROR "sse2 is not supported")
endif ()

link_libraries(pthread numa jemalloc tbb)
add_compile_options(-march=native) #-fopt-info-vec-optimized)

add_subdirectory(unit)
add_executable(SlabStore main.cpp)