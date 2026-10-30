add_executable(hello-world main.cpp)
target_link_libraries(hello-world PRIVATE cppboot::core)
cppboot_apply_compiler_options(hello-world)
