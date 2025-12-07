##################################### mytest #####################################
set(CMAKE_CXX_STANDARD 20)

add_library(plush SHARED ./plush/src/hashtable/Hashtable.cpp)
target_compile_options(plush PRIVATE -mavx512f -mavx512vl)

add_custom_target(rocksdb COMMAND make ROCKSDB_ON_DCPMM=1 shared_lib -j 32
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/test/pmem-rocksdb)

function(add_mytest name files)
    add_executable(test-${name} ${files})
    add_dependencies(test-${name} rocksdb)
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
##################################### mytest #####################################

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

##################################### utree #####################################
add_library(pibench-utree SHARED libs/libutree.cpp)
target_link_libraries(pibench-utree pmemobj pmem)
##################################### utree #####################################

##################################### dash #####################################
include(FetchContent)
include(ExternalProject)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-long-long -fPIC -march=native")

set(PMDK_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/pmdk")
ExternalProject_Add(PMDK
        GIT_REPOSITORY https://gitee.com/spearNeil/pmdk-addr-patch.git
#        GIT_REPOSITORY git@gitee.com:spearNeil/pmdk-addr-patch.git
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
#        GIT_REPOSITORY git@gitee.com:spearNeil/epoch-reclaimer.git
        GIT_TAG master
)
if (NOT epoch_reclaimer_POPULATED)
    FetchContent_Populate(epoch_reclaimer)
endif ()

add_library(pibench-dash SHARED libs/libdash.cpp)
add_dependencies(pibench-dash PMDK)
target_compile_definitions(pibench-dash PRIVATE -DPMEM)
target_link_libraries(pibench-dash pthread gflags pmemobj pmem)
target_include_directories(pibench-dash PRIVATE ${PMDK_PREFIX}/src/PMDK/src/include)
if (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    target_link_directories(pibench-dash PRIVATE ${PMDK_PREFIX}/src/PMDK/src/debug)
else ()
    target_link_directories(pibench-dash PRIVATE ${PMDK_PREFIX}/src/PMDK/src/nondebug)
endif ()
target_include_directories(pibench-dash PRIVATE ${epoch_reclaimer_SOURCE_DIR})
target_include_directories(pibench-dash PRIVATE ${PROJECT_SOURCE_DIR}/test/dash/src)
##################################### dash #####################################

##################################### FAST+FAIR #####################################
set(CMAKE_CXX_STANDARD 11)
add_library(pibench-fastfair SHARED libs/libfastfair.cpp)
target_include_directories(pibench-fastfair PRIVATE fast-fair/concurrent_pmdk/src)
target_link_libraries(pibench-fastfair rt m pthread pmemobj)
##################################### FAST+FAIR #####################################

##################################### fptree #####################################
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(pibench-fptree SHARED libs/libfptree.cpp fptree/fptree.cpp)
target_compile_options(pibench-fptree PRIVATE -march=native)# -fopt-info-vec-optimized)
target_compile_definitions(pibench-fptree PRIVATE -DPMEM)
target_include_directories(pibench-fptree PRIVATE fptree)
target_link_libraries(pibench-fptree pmemobj pmem jemalloc tbb pthread)
##################################### fptree #####################################

##################################### lbtree #####################################
# unfortunately, it does not work, so weird, xbegin always return 0
# but the test program works (unit-rtm-test)
set(CMAKE_CXX_STANDARD 11)

set(liblbtree-src lbtree/lbtree-src/lbtree.cc lbtree/common/tree.cc
        lbtree/common/mempool.cc lbtree/common/nvm-common.cc)
add_library(liblbtree SHARED ${liblbtree-src})
target_compile_options(liblbtree PUBLIC -mrtm -msse4.1 -mavx2)
target_include_directories(liblbtree PUBLIC lbtree/common lbtree/lbtree-src)
target_link_libraries(liblbtree pmem pthread)

add_library(pibench-lbtree SHARED libs/liblbtree.cpp)
target_link_libraries(pibench-lbtree liblbtree)
##################################### lbtree #####################################

set(CMAKE_CXX_STANDARD 20)
add_library(pibench-slabstore SHARED libs/libslabstore.cpp)
add_library(pibench-slabstore-var SHARED libs/libslabstore.cpp)

add_library(pibench-viper SHARED libs/libviper.cpp)
add_library(pibench-viper-var SHARED libs/libviper-var.cpp)
target_include_directories(pibench-viper PRIVATE concurrentqueue)
target_include_directories(pibench-viper-var PRIVATE concurrentqueue)

add_library(pibench-plush SHARED libs/libplush.cpp)
add_library(pibench-plush-var SHARED libs/libplush-var.cpp)
target_link_libraries(pibench-plush plush)
target_link_libraries(pibench-plush-var plush)

add_library(pibench-pmemkv SHARED libs/libpmemkv.cpp)
add_library(pibench-pmemkv-var SHARED libs/libpmemkv.cpp)
target_link_libraries(pibench-pmemkv pmemkv)
target_link_libraries(pibench-pmemkv-var pmemkv)

add_library(pibench-rocksdb-var SHARED libs/librocksdb.cpp)
add_dependencies(pibench-rocksdb-var rocksdb)
target_compile_definitions(pibench-rocksdb-var PUBLIC -DON_DCPMM)
target_link_libraries(pibench-rocksdb-var ${PROJECT_SOURCE_DIR}/test/pmem-rocksdb/librocksdb.so)
target_include_directories(pibench-rocksdb-var PRIVATE pmem-rocksdb/include)