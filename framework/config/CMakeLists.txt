add_library(cppboot-config src/config.cpp src/yaml.cpp)
add_library(cppboot::config ALIAS cppboot-config)

target_include_directories(cppboot-config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cppboot-config
    PUBLIC cppboot::base
)

cppboot_apply_compiler_options(cppboot-config)
cppboot_install_library(cppboot-config config)
