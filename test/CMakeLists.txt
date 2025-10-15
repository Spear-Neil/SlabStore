set(CMAKE_CXX_STANDARD 20)

function(add_test name files)
    add_executable(test-${name} ${files})
    message(STATUS "add test-${name}")
endfunction()

include_directories(pmem-rocksdb/include)

add_library(plush SHARED ./plush/src/hashtable/Hashtable.cpp)
target_compile_options(plush PRIVATE -mavx512f -mavx512vl)

#add_custom_target(rocksdb COMMAND make ROCKSDB_ON_DCPMM=1 shared_lib -j 32
#        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/test/pmem-rocksdb)

include_directories(concurrentqueue)
link_libraries(pmemkv plush)

aux_source_directory(./ test_files)

foreach (file ${test_files})
    get_filename_component(name ${file} NAME_WE)
    add_test(${name} ${file})
#    add_dependencies(test-${name} rocksdb)
    target_link_libraries(test-${name} ${PROJECT_SOURCE_DIR}/test/pmem-rocksdb/librocksdb.so)
    target_compile_definitions(test-${name} PUBLIC -DON_DCPMM)
endforeach ()