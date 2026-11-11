configure_file(
    include/cppboot/base/version.hpp.in
    include/cppboot/base/version.hpp
    @ONLY
)

add_library(cppboot-base src/error.cpp src/json.cpp)
add_library(cppboot::base ALIAS cppboot-base)

target_include_directories(cppboot-base PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

cppboot_apply_compiler_options(cppboot-base)
cppboot_install_library(cppboot-base base)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/cppboot/base/version.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cppboot/base
)
