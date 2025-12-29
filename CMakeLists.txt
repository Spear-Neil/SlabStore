cmake_minimum_required(VERSION 3.10)
project(SlabStore)
set(CMAKE_CXX_STANDARD 17)

include_directories(util)

execute_process(COMMAND lscpu COMMAND grep sse2 OUTPUT_VARIABLE SSE2_ENABLE)
if (NOT SSE2_ENABLE)
    message(SEND_ERROR "sse2 is not supported")
endif ()

add_subdirectory(pcm) # build libpcm.a(PCM_STATIC)

link_libraries(pthread numa jemalloc tbb PCM_STATIC)
add_compile_options(-march=native) # -fno-omit-frame-pointer -fopt-info-vec-optimized)

add_subdirectory(unit)
add_subdirectory(test)
add_executable(SlabStore-Example main.cpp)