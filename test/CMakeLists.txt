set(CMAKE_CXX_STANDARD 20)

add_library(plush SHARED ./plush/src/hashtable/Hashtable.cpp)
target_compile_options(plush PRIVATE -mavx512f -mavx512vl)

#add_custom_target(rocksdb COMMAND make ROCKSDB_ON_DCPMM=1 shared_lib -j 32
#        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/test/pmem-rocksdb)

function(add_mytest name files)
    add_executable(test-${name} ${files})
#    add_dependencies(test-${name} rocksdb)
    target_compile_definitions(test-${name} PUBLIC -DON_DCPMM)
    target_include_directories(test-${name} PRIVATE concurrentqueue)
    target_include_directories(test-${name} PRIVATE ${PROJECT_SOURCE_DIR}/pcm/src)
    target_include_directories(test-${name} PRIVATE pmem-rocksdb/include)
    target_link_libraries(test-${name} pmemkv plush)
    target_link_libraries(test-${name} ${PROJECT_SOURCE_DIR}/test/pmem-rocksdb/librocksdb.so)
    message(STATUS "add test-${name}")
endfunction()

aux_source_directory(./ test_files)
foreach (file ${test_files})
    get_filename_component(name ${file} NAME_WE)
    add_mytest(${name} ${file})
endforeach ()

##################################### pibench #####################################
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(OpenMP REQUIRED)

add_custom_target(libpcm COMMAND make lib -j
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/test/pibench/pcm)
set(pibench-src
        ${PROJECT_SOURCE_DIR}/test/pibench/src/key_generator.cpp
        ${PROJECT_SOURCE_DIR}/test/pibench/src/library_loader.cpp
        ${PROJECT_SOURCE_DIR}/test/pibench/src/benchmark.cpp
        ${PROJECT_SOURCE_DIR}/test/pibench/src/operation_generator.cpp
        ${PROJECT_SOURCE_DIR}/test/pibench/src/value_generator.cpp)
add_library(libpibench ${pibench-src})
add_dependencies(libpibench libpcm)
target_compile_options(libpibench PRIVATE ${OpenMP_CXX_FLAGS})
target_link_libraries(libpibench PRIVATE ${OpenMP_CXX_FLAGS}
        ${PROJECT_SOURCE_DIR}/test/pibench/pcm/libPCM.a dl)
target_include_directories(libpibench PUBLIC ${PROJECT_SOURCE_DIR}/test/pibench/include
        PUBLIC ${PROJECT_SOURCE_DIR}/test/pibench/pcm)

add_executable(PiBench ${PROJECT_SOURCE_DIR}/test/pibench/src/main.cpp)
target_link_libraries(PiBench libpibench)
##################################### pibench #####################################

add_library(utree SHARED libs/libutree.cpp)
target_link_libraries(utree pmemobj pmem)

##################################### dash #####################################
include(FetchContent)
include(ExternalProject)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-long-long -fPIC -march=native")

set(PMDK_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/pmdk")
ExternalProject_Add(PMDK
        GIT_REPOSITORY https://gitee.com/spearNeil/pmdk-addr-patch.git
        GIT_TAG addr-patch
        BUILD_IN_SOURCE 1
        BUILD_COMMAND $(MAKE) EXTRA_CFLAGS=-Wno-error
        PREFIX ${PMDK_PREFIX}
        CONFIGURE_COMMAND ""
        INSTALL_COMMAND ""
        LOG_DOWNLOAD ON
        LOG_CONFIGURE ON
        LOG_BUILD ON
)
FetchContent_Declare(
        epoch_reclaimer
        GIT_REPOSITORY https://gitee.com/spearNeil/epoch-reclaimer.git
        GIT_TAG master
)
FetchContent_Populate(epoch_reclaimer)

add_library(dash SHARED libs/libdash.cpp)
add_dependencies(dash PMDK)
target_compile_definitions(dash PRIVATE -DPMEM)
target_link_libraries(dash pthread gflags pmemobj pmem)
target_include_directories(dash PRIVATE ${PMDK_PREFIX}/src/PMDK/src/include)
if (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    target_link_directories(dash PRIVATE ${PMDK_PREFIX}/src/PMDK/src/debug)
else ()
    target_link_directories(dash PRIVATE ${PMDK_PREFIX}/src/PMDK/src/nondebug)
endif ()
target_include_directories(dash PRIVATE ${epoch_reclaimer_SOURCE_DIR})
target_include_directories(dash PRIVATE ${PROJECT_SOURCE_DIR}/test/dash/src)
##################################### dash #####################################