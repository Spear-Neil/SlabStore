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