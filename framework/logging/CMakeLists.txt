add_library(cppboot-logging src/logger.cpp)
add_library(cppboot::logging ALIAS cppboot-logging)

target_include_directories(cppboot-logging PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cppboot-logging
    PUBLIC cppboot::base
    PRIVATE cppboot::platform
)

cppboot_apply_compiler_options(cppboot-logging)
cppboot_install_library(cppboot-logging logging)
