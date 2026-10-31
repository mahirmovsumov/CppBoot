add_library(cppboot-cli-lib STATIC src/cli.cpp src/package_files.cpp)
target_include_directories(cppboot-cli-lib PUBLIC include src)
target_link_libraries(cppboot-cli-lib PUBLIC
    cppboot::base
    cppboot::config
    cppboot::platform
)
cppboot_apply_compiler_options(cppboot-cli-lib)

add_executable(cppboot-cli src/main.cpp)
set_target_properties(cppboot-cli PROPERTIES OUTPUT_NAME cppboot)
target_link_libraries(cppboot-cli PRIVATE cppboot-cli-lib)
cppboot_apply_compiler_options(cppboot-cli)

install(TARGETS cppboot-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
