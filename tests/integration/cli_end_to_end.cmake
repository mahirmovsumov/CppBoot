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

# New projects check bounds in standard containers. This index is inside the vector's capacity, so
# without the check it would quietly read memory and carry on.
run_cli(code output ${WORK_DIR} create demo-checks)
expect_exit_code("${code}" 0 "create a project for checks" "${output}")
set(CHECKS_DIR ${WORK_DIR}/demo-checks)
file(WRITE ${CHECKS_DIR}/src/bug.hpp [=[
#pragma once

#include <cppboot/cppboot.hpp>
#include <vector>

class CPPBOOT_SERVICE Bug : public cppboot::Module {
public:
    std::string_view name() const noexcept override { return "bug"; }

    cppboot::Result<void> start(cppboot::ApplicationContext&) override {
        std::vector<int> values;
        values.reserve(10);
        return values[5] == 12345 ? cppboot::Result<void>{} : cppboot::Result<void>{};
    }
};
]=])
run_cli(code output ${CHECKS_DIR} run --cppboot.runtime.shutdown_after_start=true)
if(code EQUAL 0)
    message(FATAL_ERROR "run with an index past the end: expected the program to stop:\n${output}")
endif()
file(REMOVE ${CHECKS_DIR}/src/bug.hpp)

# The generator warns about shared state with no lock, and still builds.
file(WRITE ${CHECKS_DIR}/src/counter.hpp [=[
#pragma once

#include <cppboot/base/annotations.hpp>

class CPPBOOT_SERVICE Counter {
public:
    void hit() { ++hits_; }

private:
    int hits_{0};
};
]=])
run_cli(code output ${CHECKS_DIR} build)
expect_exit_code("${code}" 0 "build with a warning" "${output}")
expect_contains("${output}" "warning: Field 'hits_' of Counter can change" "build with a warning")
file(REMOVE ${CHECKS_DIR}/src/counter.hpp)

# A component that keeps a pointer to a request fails the build.
file(WRITE ${CHECKS_DIR}/src/keeper.hpp [=[
#pragma once

#include <cppboot/base/annotations.hpp>
#include <cppboot/web/http.hpp>

class CPPBOOT_SERVICE Keeper {
private:
    const cppboot::web::Request* last_{nullptr};
};
]=])
run_cli(code output ${CHECKS_DIR} build)
if(code EQUAL 0)
    message(FATAL_ERROR "build with a stored request: expected a failure:\n${output}")
endif()
expect_contains("${output}" "ends with its request" "build with a stored request")
file(REMOVE ${CHECKS_DIR}/src/keeper.hpp)

run_cli(code output ${CHECKS_DIR} build --sanitize=memory)
expect_exit_code("${code}" 2 "build with an unknown checker" "${output}")

if(WIN32)
    run_cli(code output ${CHECKS_DIR} build --sanitize=address)
    expect_exit_code("${code}" 1 "--sanitize on Windows" "${output}")
    expect_contains("${output}" "needs Linux" "--sanitize on Windows")
elseif(NOT "$ENV{CXXFLAGS}" MATCHES "fsanitize")
    # Each checker has to find a real bug in a project, through the CLI.
    file(WRITE ${CHECKS_DIR}/src/freed.hpp [=[
#pragma once

#include <cppboot/cppboot.hpp>

class CPPBOOT_SERVICE Freed : public cppboot::Module {
public:
    std::string_view name() const noexcept override { return "freed"; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        int* volatile value = new int{1};
        delete value;
        context.logger().info("{}", *value);
        return {};
    }
};
]=])
    run_cli(code output ${CHECKS_DIR} run --sanitize=address --cppboot.runtime.shutdown_after_start=true)
    if(code EQUAL 0)
        message(FATAL_ERROR "--sanitize=address: expected a failure:\n${output}")
    endif()
    expect_contains("${output}" "heap-use-after-free" "--sanitize=address")
    file(REMOVE ${CHECKS_DIR}/src/freed.hpp)

    file(WRITE ${CHECKS_DIR}/src/racy.hpp [=[
#pragma once

#include <cppboot/cppboot.hpp>
#include <thread>

class CPPBOOT_SERVICE Racy : public cppboot::Module {
public:
    std::string_view name() const noexcept override { return "racy"; }

    cppboot::Result<void> start(cppboot::ApplicationContext&) override {
        int shared = 0;
        {
            std::jthread first{[&shared] { for (int i = 0; i < 100000; ++i) { ++shared; } }};
            std::jthread second{[&shared] { for (int i = 0; i < 100000; ++i) { ++shared; } }};
        }
        return {};
    }
};
]=])
    run_cli(code output ${CHECKS_DIR} run --sanitize=thread --cppboot.runtime.shutdown_after_start=true)
    expect_contains("${output}" "data race" "--sanitize=thread")
    file(REMOVE ${CHECKS_DIR}/src/racy.hpp)
endif()

# The generator has to read headers as the compiler does: with the target's definitions and options.
run_cli(code output ${WORK_DIR} create demo-context)
expect_exit_code("${code}" 0 "create for compiler context" "${output}")
set(CONTEXT_DIR ${WORK_DIR}/demo-context)
file(APPEND ${CONTEXT_DIR}/CMakeLists.txt "
target_compile_definitions(demo-context PRIVATE FEATURE_FROM_DEFINITIONS)
target_compile_options(demo-context PRIVATE -DFEATURE_FROM_OPTIONS -Wall)
")
foreach(feature FEATURE_FROM_DEFINITIONS FEATURE_FROM_OPTIONS)
    file(WRITE ${CONTEXT_DIR}/src/${feature}.hpp "#pragma once
#include <cppboot/cppboot.hpp>
#ifdef ${feature}
class CPPBOOT_SERVICE ${feature}_Module : public cppboot::Module {
public:
    std::string_view name() const noexcept override { return \"${feature}\"; }
    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        context.logger().info(\"Found ${feature}\");
        return {};
    }
};
#endif
")
endforeach()

# The generated file depends on a setting in a header outside src, which the generator has to notice.
file(WRITE ${CONTEXT_DIR}/settings.hpp "#pragma once\n#define WITH_OPTIONAL_MODULE 1\n")
file(WRITE ${CONTEXT_DIR}/src/optional_module.hpp "#pragma once
#include <cppboot/cppboot.hpp>
#include \"../settings.hpp\"
#if WITH_OPTIONAL_MODULE
class CPPBOOT_SERVICE OptionalModule : public cppboot::Module {
public:
    std::string_view name() const noexcept override { return \"optional\"; }
    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        context.logger().info(\"Optional module is on\");
        return {};
    }
};
#endif
")
run_cli(code output ${CONTEXT_DIR} run --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run with compiler context" "${output}")
expect_contains("${output}" "Found FEATURE_FROM_DEFINITIONS" "a definition reaches the generator")
expect_contains("${output}" "Found FEATURE_FROM_OPTIONS" "an option reaches the generator")
expect_contains("${output}" "Optional module is on" "an included setting reaches the generator")

# Changing only the included file must read that header again. Without that, the generated file
# would still register OptionalModule, which no longer exists, and the build would fail.
file(WRITE ${CONTEXT_DIR}/settings.hpp "#pragma once\n#define WITH_OPTIONAL_MODULE 0\n")
run_cli(code output ${CONTEXT_DIR} run --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "run after an included file changed" "${output}")
string(FIND "${output}" "Optional module is on" optional_position)
if(NOT optional_position EQUAL -1)
    message(FATAL_ERROR "an included file changed, but the generator didn't notice:\n${output}")
endif()

# Changing one header reads only that one again.
file(APPEND ${CONTEXT_DIR}/src/greeting.hpp "\n// Changed.\n")
run_cli(code output ${CONTEXT_DIR} build)
expect_exit_code("${code}" 0 "build after one header changed" "${output}")
string(REGEX MATCHALL "Reading CppBoot marks in [^\n]+" reads "${output}")
list(LENGTH reads read_count)
if(NOT read_count EQUAL 1)
    # Shows why, since this has differed between platforms.
    file(GLOB_RECURSE depfiles ${CONTEXT_DIR}/build/*.json.d)
    list(GET depfiles 0 first_depfile)
    file(READ ${first_depfile} depfile_text)
    file(APPEND ${CONTEXT_DIR}/src/greeting.hpp "\n// Changed again.\n")
    execute_process(COMMAND ninja -d explain WORKING_DIRECTORY ${CONTEXT_DIR}/build
                    OUTPUT_VARIABLE explained ERROR_VARIABLE explained)
    message(FATAL_ERROR "one header changed, but ${read_count} were read again:\n${output}\n"
                        "A dependency file, ${first_depfile}:\n${depfile_text}\n"
                        "Ninja's reasons on the next build:\n${explained}")
endif()
expect_contains("${output}" "Reading CppBoot marks in greeting.hpp" "build after one header changed")

# The code in the tutorials and guides has to work as printed, or it goes stale without anyone noticing.
# This returns the first code block after a piece of text in a document.
function(read_code_block document marker result_var)
    file(READ ${SOURCE_DIR}/docs/${document} text)
    string(FIND "${text}" "${marker}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${document} doesn't contain '${marker}'")
    endif()
    string(SUBSTRING "${text}" ${position} -1 text)
    string(FIND "${text}" "```cpp\n" position)
    math(EXPR position "${position} + 7")
    string(SUBSTRING "${text}" ${position} -1 text)
    string(FIND "${text}" "```" position)
    string(SUBSTRING "${text}" 0 ${position} code)
    set(${result_var} "${code}" PARENT_SCOPE)
endfunction()

# Getting started: the component it adds.
run_cli(code output ${WORK_DIR} create tutorial-start)
expect_exit_code("${code}" 0 "create for the first tutorial" "${output}")
read_code_block(tutorials/getting-started.md "Create `src/clock.hpp`" clock_code)
file(WRITE ${WORK_DIR}/tutorial-start/src/clock.hpp "${clock_code}")
run_cli(code output ${WORK_DIR}/tutorial-start run --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "the first tutorial's component" "${output}")
expect_contains("${output}" "Hello from CppBoot! Good morning." "the first tutorial's component")

# Getting started: the change it makes to that component.
read_code_block(tutorials/getting-started.md "replace the line that logs" setting_code)
file(READ ${WORK_DIR}/tutorial-start/src/clock.hpp clock_text)
string(REPLACE [=[context.logger().info("{} Good {}.", greetings_.greeting(), time_.period());]=]
       "${setting_code}" clock_text "${clock_text}")
file(WRITE ${WORK_DIR}/tutorial-start/src/clock.hpp "${clock_text}")
run_cli(code output ${WORK_DIR}/tutorial-start run
    --cppboot.runtime.shutdown_after_start=true --greeting.name=Alex)
expect_exit_code("${code}" 0 "the first tutorial's setting" "${output}")
expect_contains("${output}" "Good morning, world." "the first tutorial's setting")

# The REST API tutorial, which replaces a whole file. The build must also stay free of warnings
# about its store, because it's what the tutorial teaches.
run_cli(code output ${WORK_DIR} create tutorial-api --web)
expect_exit_code("${code}" 0 "create for the REST tutorial" "${output}")
read_code_block(tutorials/rest-api.md "Replace `src/users.hpp` with this" users_code)
file(WRITE ${WORK_DIR}/tutorial-api/src/users.hpp "${users_code}")
run_cli(code output ${WORK_DIR}/tutorial-api run
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "the REST tutorial" "${output}")
string(FIND "${output}" "warning: Field" warning_position)
if(NOT warning_position EQUAL -1)
    message(FATAL_ERROR "the REST tutorial: its code gets a warning:\n${output}")
endif()

# The security guide's middleware, in a project that uses it.
run_cli(code output ${WORK_DIR} create guide-limit --web)
expect_exit_code("${code}" 0 "create for the rate limit guide" "${output}")
read_code_block(guides/security.md "### Limit every route" limit_code)
file(WRITE ${WORK_DIR}/guide-limit/src/api_limit.hpp "#pragma once
#include <chrono>
#include <cppboot/cppboot.hpp>
#include <cppboot/web/rate_limit.hpp>
${limit_code}")
run_cli(code output ${WORK_DIR}/guide-limit run
    --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true)
expect_exit_code("${code}" 0 "the rate limit guide's middleware" "${output}")

# A package has to run from another folder, with no build folder and no SDK to rely on.
run_cli(code output ${WORK_DIR}/demo-store package)
expect_exit_code("${code}" 0 "package" "${output}")
expect_contains("${output}" "Packaged " "package")
set(STAGE ${WORK_DIR}/demo-store/dist/demo-store)
foreach(file bin/demo-store${EXE_SUFFIX} cppboot.yaml db/migrations/001_create_users.sql README.md)
    if(NOT EXISTS ${STAGE}/${file})
        message(FATAL_ERROR "package: ${file} is missing")
    endif()
endforeach()

set(MOVED_PACKAGE ${WORK_DIR}/elsewhere/demo-store)
file(REMOVE_RECURSE ${WORK_DIR}/elsewhere)
file(COPY ${STAGE} DESTINATION ${WORK_DIR}/elsewhere)
execute_process(
    COMMAND ${MOVED_PACKAGE}/bin/demo-store${EXE_SUFFIX}
        --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true
    WORKING_DIRECTORY ${MOVED_PACKAGE}
    RESULT_VARIABLE code
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
expect_exit_code("${code}" 0 "run a package" "${output}")
expect_contains("${output}" "Applied 1 new database migrations" "run a package")

if(WIN32)
    set(ARCHIVE ${WORK_DIR}/demo-store/dist/demo-store-windows.zip)
else()
    set(ARCHIVE ${WORK_DIR}/demo-store/dist/demo-store-linux.tar.gz)
    foreach(file demo-store.service demo-store.env.example install.sh uninstall.sh lib)
        if(NOT EXISTS ${STAGE}/${file})
            message(FATAL_ERROR "package: ${file} is missing")
        endif()
    endforeach()
    execute_process(COMMAND sh -n ${STAGE}/install.sh COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND sh -n ${STAGE}/uninstall.sh COMMAND_ERROR_IS_FATAL ANY)
endif()
if(NOT EXISTS ${ARCHIVE})
    message(FATAL_ERROR "package: ${ARCHIVE} is missing")
endif()

run_cli(code output ${WORK_DIR} package)
expect_exit_code("${code}" 1 "package outside a project" "${output}")

run_cli(code output ${WORK_DIR} build)
expect_exit_code("${code}" 1 "build outside a project" "${output}")
expect_contains("${output}" "No CppBoot project here" "build outside a project")
