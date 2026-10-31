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

# A web project has to build against the installed SDK and start its server.
run_cli(code output ${WORK_DIR} create demo-api --web)
expect_exit_code("${code}" 0 "create --web" "${output}")
run_cli(code output ${WORK_DIR}/demo-api run
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run a web project" "${output}")
expect_contains("${output}" "Server listening on port" "run a web project")

# A database project has to apply its migration on the first run, and only then.
run_cli(code output ${WORK_DIR} create demo-store --web --sqlite)
expect_exit_code("${code}" 0 "create --sqlite" "${output}")
run_cli(code output ${WORK_DIR}/demo-store run
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run a database project" "${output}")
expect_contains("${output}" "Applied 1 new database migrations" "run a database project")
expect_contains("${output}" "Server listening on port" "run a database project")
run_cli(code output ${WORK_DIR}/demo-store run
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run a database project again" "${output}")
expect_contains("${output}" "Applied 0 new database migrations" "run a database project again")

# A database that can't be reached has to stop startup, without showing the password.
run_cli(code output ${WORK_DIR}/demo-store run
    "--cppboot.database.url=postgresql://user:hunter2@127.0.0.1:1/absent?connect_timeout=2"
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
if(code EQUAL 0)
    message(FATAL_ERROR "run with a bad database: expected a failure:\n${output}")
endif()
string(FIND "${output}" "hunter2" password_position)
if(NOT password_position EQUAL -1)
    message(FATAL_ERROR "run with a bad database: the output shows the password:\n${output}")
endif()

# A secured project must refuse to start without its secret, and start with one.
run_cli(code output ${WORK_DIR} create demo-safe --web --security)
expect_exit_code("${code}" 0 "create --security" "${output}")
set(ENV{CPPBOOT_MYAPP_PASSWORD} "end-to-end-password")
run_cli(code output ${WORK_DIR}/demo-safe run
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
if(code EQUAL 0)
    message(FATAL_ERROR "run a secured project with no secret: expected a failure:\n${output}")
endif()
expect_contains("${output}" "CPPBOOT_SECURITY_JWT_SECRET" "run a secured project with no secret")

set(ENV{CPPBOOT_SECURITY_JWT_SECRET} "an-end-to-end-secret-that-is-long-enough")
run_cli(code output ${WORK_DIR}/demo-safe run
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run a secured project" "${output}")
expect_contains("${output}" "Server listening on port" "run a secured project")

# Every option together has to build and start. The plan calls this target "banking-api".
run_cli(code output ${WORK_DIR} create banking-api --web --sqlite --security --actuator)
expect_exit_code("${code}" 0 "create with every option" "${output}")
run_cli(code output ${WORK_DIR}/banking-api run
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run with every option" "${output}")
expect_contains("${output}" "Applied 1 new database migrations" "run with every option")
expect_contains("${output}" "Server listening on port" "run with every option")

# The actuator alone has to give a plain project a server.
run_cli(code output ${WORK_DIR} create demo-watched --actuator)
expect_exit_code("${code}" 0 "create --actuator" "${output}")
run_cli(code output ${WORK_DIR}/demo-watched run
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run with the actuator" "${output}")
expect_contains("${output}" "Hello from CppBoot!" "run with the actuator")
expect_contains("${output}" "Server listening on port" "run with the actuator")

unset(ENV{CPPBOOT_SECURITY_JWT_SECRET})
unset(ENV{CPPBOOT_MYAPP_PASSWORD})

run_cli(code output ${WORK_DIR} build)
expect_exit_code("${code}" 1 "build outside a project" "${output}")
expect_contains("${output}" "No CppBoot project here" "build outside a project")
