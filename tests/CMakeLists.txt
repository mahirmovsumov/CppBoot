find_package(GTest CONFIG REQUIRED)

# Reports what it received, so the process tests can check what runProcess passed along.
add_executable(cppboot-test-child helpers/process_child.cpp)
cppboot_apply_compiler_options(cppboot-test-child)

add_executable(cppboot-unit-tests
    unit/application_test.cpp
    unit/cli_test.cpp
    unit/config_test.cpp
    unit/lifecycle_test.cpp
    unit/module_manager_test.cpp
    unit/process_test.cpp
    unit/result_test.cpp
    unit/signals_test.cpp
    unit/worker_group_test.cpp
)

target_link_libraries(cppboot-unit-tests PRIVATE
    cppboot::core
    cppboot-cli-lib
    GTest::gtest_main
)
target_compile_definitions(cppboot-unit-tests PRIVATE
    CPPBOOT_TEST_CHILD="$<TARGET_FILE:cppboot-test-child>"
)
add_dependencies(cppboot-unit-tests cppboot-test-child)

cppboot_apply_compiler_options(cppboot-unit-tests)

add_test(NAME cppboot-unit-tests COMMAND cppboot-unit-tests)

# Installs the framework, then creates, builds and runs a project using the installed CLI.
add_test(NAME cppboot-cli-end-to-end
    COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${PROJECT_BINARY_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/end-to-end
        -DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
        -DEXPECTED_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_end_to_end.cmake
)
# Sanitizer flags have to reach the generated project too, or it won't link.
set_tests_properties(cppboot-cli-end-to-end PROPERTIES
    ENVIRONMENT "CXXFLAGS=${CMAKE_CXX_FLAGS}"
    TIMEOUT 600
)
