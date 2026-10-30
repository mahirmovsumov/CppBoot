add_executable(rest-api main.cpp)
target_link_libraries(rest-api PRIVATE cppboot::web)
cppboot_apply_compiler_options(rest-api)
