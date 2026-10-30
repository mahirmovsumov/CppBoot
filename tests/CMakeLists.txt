find_package(GTest CONFIG REQUIRED)

add_executable(cppboot-unit-tests
    unit/application_test.cpp
    unit/config_test.cpp
    unit/lifecycle_test.cpp
    unit/module_manager_test.cpp
    unit/result_test.cpp
    unit/signals_test.cpp
    unit/worker_group_test.cpp
)

target_link_libraries(cppboot-unit-tests PRIVATE
    cppboot::core
    GTest::gtest_main
)

cppboot_apply_compiler_options(cppboot-unit-tests)

add_test(NAME cppboot-unit-tests COMMAND cppboot-unit-tests)
