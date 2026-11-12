find_package(GTest CONFIG REQUIRED)

# Reports what it received, so the process tests can check what runProcess passed along.
add_executable(cppboot-test-child helpers/process_child.cpp)
target_link_libraries(cppboot-test-child PRIVATE cppboot::platform)
cppboot_apply_compiler_options(cppboot-test-child)

add_executable(cppboot-unit-tests
    unit/application_test.cpp
    unit/binding_test.cpp
    unit/boot_test.cpp
    unit/cli_test.cpp
    unit/config_test.cpp
    unit/di_test.cpp
    unit/json_test.cpp
    unit/lifecycle_test.cpp
    unit/logger_test.cpp
    unit/module_manager_test.cpp
    unit/process_test.cpp
    unit/result_test.cpp
    unit/signals_test.cpp
    unit/validation_test.cpp
    unit/worker_group_test.cpp
    unit/yaml_test.cpp
)

target_link_libraries(cppboot-unit-tests PRIVATE
    cppboot::boot
    cppboot-cli-lib
    GTest::gtest_main
)
target_compile_definitions(cppboot-unit-tests PRIVATE
    CPPBOOT_TEST_CHILD="$<TARGET_FILE:cppboot-test-child>"
    CPPBOOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
add_dependencies(cppboot-unit-tests cppboot-test-child)
# For parsers that are internal to their libraries.
target_include_directories(cppboot-unit-tests PRIVATE ${PROJECT_SOURCE_DIR}/framework/config/src)

cppboot_apply_compiler_options(cppboot-unit-tests)

add_test(NAME cppboot-unit-tests COMMAND cppboot-unit-tests)

add_executable(cppboot-web-tests
    web/rate_limit_test.cpp
    web/router_test.cpp
    web/server_test.cpp
    web/tls_test.cpp
)
target_link_libraries(cppboot-web-tests PRIVATE
    cppboot::web
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    GTest::gtest_main
)
if(WIN32)
    target_compile_definitions(cppboot-web-tests PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(cppboot-web-tests PRIVATE ws2_32 mswsock)
endif()
cppboot_apply_compiler_options(cppboot-web-tests)
add_test(NAME cppboot-web-tests COMMAND cppboot-web-tests)
# The same tests again, with connections shared among several network threads.
add_test(NAME cppboot-web-tests-many-io-threads COMMAND cppboot-web-tests)
set_tests_properties(cppboot-web-tests-many-io-threads PROPERTIES
    ENVIRONMENT "CPPBOOT_TEST_IO_THREADS=4"
)

add_executable(cppboot-security-tests security/security_test.cpp)
target_link_libraries(cppboot-security-tests PRIVATE
    cppboot::security
    OpenSSL::Crypto
    GTest::gtest_main
)
cppboot_apply_compiler_options(cppboot-security-tests)
add_test(NAME cppboot-security-tests COMMAND cppboot-security-tests)

add_executable(cppboot-actuator-tests actuator/actuator_test.cpp)
target_link_libraries(cppboot-actuator-tests PRIVATE
    cppboot::actuator
    GTest::gtest_main
)
cppboot_apply_compiler_options(cppboot-actuator-tests)
add_test(NAME cppboot-actuator-tests COMMAND cppboot-actuator-tests)

# Every public header has to compile by itself, so that its own includes are complete. Nothing runs
# this: building it is the test.
file(GLOB public_headers CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/framework/*/include/cppboot/*.hpp
    ${PROJECT_SOURCE_DIR}/framework/*/include/cppboot/*/*.hpp
)
set(header_test_sources)
foreach(header IN LISTS public_headers)
    string(MD5 header_id ${header})
    set(source ${CMAKE_CURRENT_BINARY_DIR}/header-tests/${header_id}.cpp)
    file(GENERATE OUTPUT ${source} CONTENT "#include \"${header}\"\n")
    list(APPEND header_test_sources ${source})
endforeach()
add_library(cppboot-header-tests OBJECT ${header_test_sources})
target_link_libraries(cppboot-header-tests PRIVATE
    cppboot::boot
    cppboot::web
    cppboot::data
    cppboot::security
    cppboot::actuator
)
cppboot_apply_compiler_options(cppboot-header-tests)

# PostgreSQL tests run only if CPPBOOT_TEST_POSTGRESQL_URL names a test database.
add_executable(cppboot-data-tests data/data_test.cpp)
target_link_libraries(cppboot-data-tests PRIVATE
    cppboot::data
    GTest::gtest_main
)
cppboot_apply_compiler_options(cppboot-data-tests)
add_test(NAME cppboot-data-tests COMMAND cppboot-data-tests)

if(CPPBOOT_BUILD_CODEGEN)
    add_executable(cppboot-codegen-tests
        codegen/model_test.cpp
        codegen/parser_test.cpp
        codegen/scan_test.cpp
    )
    target_link_libraries(cppboot-codegen-tests PRIVATE
        cppboot-codegen-lib
        GTest::gtest_main
    )
    cppboot_apply_compiler_options(cppboot-codegen-tests)
    add_test(NAME cppboot-codegen-tests COMMAND cppboot-codegen-tests)

    # Runs the generator during the build, then compiles and runs what it generated.
    add_executable(cppboot-generated-wiring-tests codegen/wiring_test.cpp)
    target_link_libraries(cppboot-generated-wiring-tests PRIVATE
        cppboot::boot
        GTest::gtest_main
    )
    cppboot_generate_components(cppboot-generated-wiring-tests
        HEADERS codegen/sample_components.hpp
    )
    cppboot_apply_compiler_options(cppboot-generated-wiring-tests)
    add_test(NAME cppboot-generated-wiring-tests COMMAND cppboot-generated-wiring-tests)

    add_executable(cppboot-generated-controller-tests codegen/controller_test.cpp)
    target_link_libraries(cppboot-generated-controller-tests PRIVATE
        cppboot::boot
        cppboot::web
        GTest::gtest_main
    )
    cppboot_generate_components(cppboot-generated-controller-tests
        HEADERS codegen/sample_controller.hpp
    )
    cppboot_apply_compiler_options(cppboot-generated-controller-tests)
    add_test(NAME cppboot-generated-controller-tests COMMAND cppboot-generated-controller-tests)

    add_executable(cppboot-generated-repository-tests codegen/repository_test.cpp)
    target_link_libraries(cppboot-generated-repository-tests PRIVATE
        cppboot::boot
        cppboot::data
        cppboot::web
        GTest::gtest_main
    )
    cppboot_generate_components(cppboot-generated-repository-tests
        HEADERS codegen/sample_repository.hpp
    )
    cppboot_apply_compiler_options(cppboot-generated-repository-tests)
    add_test(NAME cppboot-generated-repository-tests COMMAND cppboot-generated-repository-tests)

    add_executable(cppboot-generated-secured-tests codegen/secured_test.cpp)
    target_link_libraries(cppboot-generated-secured-tests PRIVATE
        cppboot::boot
        cppboot::security
        cppboot::web
        GTest::gtest_main
    )
    cppboot_generate_components(cppboot-generated-secured-tests
        HEADERS codegen/sample_secured.hpp
    )
    cppboot_apply_compiler_options(cppboot-generated-secured-tests)
    add_test(NAME cppboot-generated-secured-tests COMMAND cppboot-generated-secured-tests)

    add_executable(cppboot-generated-monitored-tests codegen/monitored_test.cpp)
    target_link_libraries(cppboot-generated-monitored-tests PRIVATE
        cppboot::boot
        GTest::gtest_main
    )
    cppboot_generate_components(cppboot-generated-monitored-tests
        ACTUATOR
        HEADERS codegen/sample_monitored.hpp
    )
    cppboot_apply_compiler_options(cppboot-generated-monitored-tests)
    add_test(NAME cppboot-generated-monitored-tests COMMAND cppboot-generated-monitored-tests)

    add_executable(cppboot-generated-settings-tests codegen/settings_test.cpp)
    target_link_libraries(cppboot-generated-settings-tests PRIVATE
        cppboot::boot
        cppboot::web
        GTest::gtest_main
    )
    cppboot_generate_components(cppboot-generated-settings-tests
        HEADERS codegen/sample_settings.hpp
    )
    cppboot_apply_compiler_options(cppboot-generated-settings-tests)
    add_test(NAME cppboot-generated-settings-tests COMMAND cppboot-generated-settings-tests)

    add_executable(cppboot-generated-queries-tests codegen/queries_test.cpp)
    target_link_libraries(cppboot-generated-queries-tests PRIVATE
        cppboot::boot
        cppboot::data
        GTest::gtest_main
    )
    cppboot_generate_components(cppboot-generated-queries-tests
        HEADERS codegen/sample_queries.hpp
    )
    cppboot_apply_compiler_options(cppboot-generated-queries-tests)
    add_test(NAME cppboot-generated-queries-tests COMMAND cppboot-generated-queries-tests)

    # Installs the framework, then creates, builds and runs a project using the installed CLI.
    add_test(NAME cppboot-cli-end-to-end
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${PROJECT_BINARY_DIR}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/end-to-end
            -DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
            -DEXPECTED_VERSION=${PROJECT_VERSION}
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_end_to_end.cmake
    )
    # Sanitizer flags have to reach the generated project too, or it won't link.
    set_tests_properties(cppboot-cli-end-to-end PROPERTIES
        ENVIRONMENT "CXXFLAGS=${CMAKE_CXX_FLAGS}"
        TIMEOUT 1200
    )
endif()
