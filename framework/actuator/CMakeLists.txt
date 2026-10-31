add_library(cppboot-actuator
    src/actuator.cpp
    src/metrics.cpp
)
add_library(cppboot::actuator ALIAS cppboot-actuator)

target_include_directories(cppboot-actuator PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cppboot-actuator
    PUBLIC
        cppboot::base
        cppboot::config
        cppboot::logging
        cppboot::web
    PRIVATE
        cppboot::platform
)

cppboot_apply_compiler_options(cppboot-actuator)
cppboot_install_library(cppboot-actuator actuator)
