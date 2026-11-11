# Fuzz targets for the parsers that read text CppBoot didn't write. They need Clang's libFuzzer, and
# the libraries need coverage too, so that it can find paths inside them:
#
#   cmake -S . -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCPPBOOT_BUILD_FUZZERS=ON \
#     -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined"
#   cmake --build build/fuzz --target cppboot-fuzzers
#   build/fuzz/tests/fuzz/cppboot-yaml-fuzz -max_total_time=60 <a copy of tests/fuzz/corpus/yaml>
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "CPPBOOT_BUILD_FUZZERS needs Clang, for libFuzzer")
endif()

add_custom_target(cppboot-fuzzers)

function(cppboot_add_fuzzer name source)
    add_executable(${name} ${source})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(${name} PRIVATE ${ARGN})
    cppboot_apply_compiler_options(${name})
    add_dependencies(cppboot-fuzzers ${name})
endfunction()

cppboot_add_fuzzer(cppboot-yaml-fuzz yaml_fuzz.cpp cppboot::config)
target_include_directories(cppboot-yaml-fuzz PRIVATE ${PROJECT_SOURCE_DIR}/framework/config/src)

cppboot_add_fuzzer(cppboot-json-fuzz json_fuzz.cpp cppboot::base)
