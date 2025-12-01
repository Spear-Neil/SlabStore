function(add_unit name files)
    add_executable(unit-${name} ${files})
    target_include_directories(unit-${name} PRIVATE ${PROJECT_SOURCE_DIR}/pcm/src)
    message(STATUS "add unit-${name}")
endfunction()

aux_source_directory(./ unit_test_files)
foreach (file ${unit_test_files})
    get_filename_component(name ${file} NAME_WE)
    add_unit(${name} ${file})
endforeach ()

target_compile_options(unit-rtm-test PRIVATE -mrtm)
