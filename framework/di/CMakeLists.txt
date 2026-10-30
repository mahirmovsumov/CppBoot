add_library(cppboot-di src/container.cpp)
add_library(cppboot::di ALIAS cppboot-di)

target_include_directories(cppboot-di PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cppboot-di PUBLIC cppboot::base)

cppboot_apply_compiler_options(cppboot-di)
cppboot_install_library(cppboot-di di)
