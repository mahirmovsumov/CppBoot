add_library(cppboot-boot src/boot.cpp)
add_library(cppboot::boot ALIAS cppboot-boot)

target_include_directories(cppboot-boot PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cppboot-boot PUBLIC
    cppboot::core
    cppboot::di
)

cppboot_apply_compiler_options(cppboot-boot)
cppboot_install_library(cppboot-boot boot)
