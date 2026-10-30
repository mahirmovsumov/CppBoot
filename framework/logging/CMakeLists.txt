add_library(cppboot-logging src/logger.cpp)
add_library(cppboot::logging ALIAS cppboot-logging)

target_include_directories(cppboot-logging PUBLIC include)
target_link_libraries(cppboot-logging
    PUBLIC cppboot::base
    PRIVATE spdlog::spdlog
)

cppboot_apply_compiler_options(cppboot-logging)
