add_library(cppboot-config src/config.cpp)
add_library(cppboot::config ALIAS cppboot-config)

target_include_directories(cppboot-config PUBLIC include)
target_link_libraries(cppboot-config
    PUBLIC cppboot::base
    PRIVATE yaml-cpp::yaml-cpp
)

cppboot_apply_compiler_options(cppboot-config)
