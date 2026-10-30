# Run with: cmake -DBUILD_DIR=... -DWORK_DIR=... -DEXE_SUFFIX=... -DEXPECTED_VERSION=... -P <this file>

function(expect_contains text expected step)
    string(FIND "${text}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${step}: output doesn't contain '${expected}':\n${text}")
    endif()
endfunction()

function(run_cli result_var output_var directory)
    execute_process(
        COMMAND ${CPPBOOT} ${ARGN}
        WORKING_DIRECTORY ${directory}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
    )
    message(STATUS "cppboot ${ARGN} -> ${result}")
    set(${result_var} "${result}" PARENT_SCOPE)
    set(${output_var} "${output}" PARENT_SCOPE)
endfunction()

function(expect_exit_code actual expected step output)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "${step}: expected exit code ${expected}, got ${actual}:\n${output}")
    endif()
endfunction()

set(PREFIX ${WORK_DIR}/prefix)
set(CPPBOOT ${PREFIX}/bin/cppboot${EXE_SUFFIX})
set(PROJECT_DIR ${WORK_DIR}/demo-app)

file(REMOVE_RECURSE ${WORK_DIR})
file(MAKE_DIRECTORY ${WORK_DIR})

execute_process(
    COMMAND ${CMAKE_COMMAND} --install ${BUILD_DIR} --prefix ${PREFIX}
    OUTPUT_QUIET
    COMMAND_ERROR_IS_FATAL ANY
)

# The installed generator must use the Clang headers installed with it.
execute_process(
    COMMAND ${PREFIX}/bin/cppboot-codegen${EXE_SUFFIX} --print-resource-dir
    OUTPUT_VARIABLE resource_dir
    COMMAND_ERROR_IS_FATAL ANY
)
expect_contains("${resource_dir}" "lib/cppboot/clang" "installed generator")

run_cli(code output ${WORK_DIR} info)
expect_exit_code("${code}" 0 "info" "${output}")
expect_contains("${output}" "CppBoot ${EXPECTED_VERSION}" "info")

run_cli(code output ${WORK_DIR} create demo-app)
expect_exit_code("${code}" 0 "create" "${output}")

run_cli(code output ${WORK_DIR} create demo-app)
expect_exit_code("${code}" 1 "create over an existing project" "${output}")
expect_contains("${output}" "already exists" "create over an existing project")

run_cli(code output ${PROJECT_DIR} build)
expect_exit_code("${code}" 0 "build" "${output}")

run_cli(code output ${PROJECT_DIR} run -- --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run" "${output}")
expect_contains("${output}" "[demo-app]" "run")
# Printed by a component that the generated code created and injected.
expect_contains("${output}" "Hello from CppBoot!" "run")
expect_contains("${output}" "Application is running" "run")
expect_contains("${output}" "Shutting down" "run")

# A new marked class in a new header has to be found and injected with no registration code.
file(WRITE ${PROJECT_DIR}/src/farewell.hpp [=[
#pragma once

#include <cppboot/cppboot.hpp>
#include <memory>
#include <utility>

#include "greeting.hpp"

class CPPBOOT_SERVICE FarewellModule : public cppboot::Module {
public:
    FarewellModule(std::shared_ptr<GreetingService> greetings,
                   std::shared_ptr<const cppboot::config::Config> config,
                   std::shared_ptr<const cppboot::logging::Logger> logger)
        : greetings_(std::move(greetings)),
          name_(config->getString("cppboot.application.name")),
          logger_(std::move(logger)) {}

    std::string_view name() const noexcept override { return "farewell"; }

    cppboot::Result<void> start(cppboot::ApplicationContext&) override {
        logger_->info("Injected in {}: {}", name_, greetings_->greeting());
        return {};
    }

private:
    std::shared_ptr<GreetingService> greetings_;
    std::string name_;
    std::shared_ptr<const cppboot::logging::Logger> logger_;
};
]=])
run_cli(code output ${PROJECT_DIR} run --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run with a new component" "${output}")
expect_contains("${output}" "Injected in demo-app: Hello from CppBoot!" "run with a new component")

# A component with a missing dependency has to fail the build, not the running application.
file(WRITE ${PROJECT_DIR}/src/broken.hpp [=[
#pragma once

#include <cppboot/base/annotations.hpp>
#include <memory>

class NotAComponent {};

class CPPBOOT_SERVICE Broken {
public:
    explicit Broken(std::shared_ptr<NotAComponent> dependency);
};
]=])
run_cli(code output ${PROJECT_DIR} build)
if(code EQUAL 0)
    message(FATAL_ERROR "build with a missing dependency: expected a failure:\n${output}")
endif()
expect_contains("${output}" "needs NotAComponent" "build with a missing dependency")
file(REMOVE ${PROJECT_DIR}/src/broken.hpp)

# The application's own exit code has to come back through the CLI.
run_cli(code output ${PROJECT_DIR} run --cppboot.logging.level=loud)
expect_exit_code("${code}" 78 "run with invalid configuration" "${output}")
expect_contains("${output}" "Unknown log level" "run with invalid configuration")

run_cli(code output ${WORK_DIR} build)
expect_exit_code("${code}" 1 "build outside a project" "${output}")
expect_contains("${output}" "No CppBoot project here" "build outside a project")
