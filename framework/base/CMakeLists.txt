configure_file(
    include/cppboot/base/version.hpp.in
    include/cppboot/base/version.hpp
    @ONLY
)

add_library(cppboot-base src/error.cpp)
add_library(cppboot::base ALIAS cppboot-base)

target_include_directories(cppboot-base PUBLIC
    include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

cppboot_apply_compiler_options(cppboot-base)
