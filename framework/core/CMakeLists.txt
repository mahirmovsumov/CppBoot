add_library(cppboot-core
    src/application.cpp
    src/application_context.cpp
    src/lifecycle.cpp
    src/module.cpp
    src/worker_group.cpp
)
add_library(cppboot::core ALIAS cppboot-core)

target_include_directories(cppboot-core PUBLIC include)
target_link_libraries(cppboot-core PUBLIC
    cppboot::base
    cppboot::config
    cppboot::logging
    cppboot::platform
)

cppboot_apply_compiler_options(cppboot-core)
