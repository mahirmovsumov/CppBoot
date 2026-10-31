cmake_minimum_required(VERSION 3.25)

project(CppBoot VERSION 0.1.0 DESCRIPTION "Native application framework for C++20" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CPPBOOT_BUILD_TESTS "Build CppBoot tests" ON)
option(CPPBOOT_BUILD_EXAMPLES "Build CppBoot examples" ON)
option(CPPBOOT_BUILD_CODEGEN "Build the code generator, which requires libclang" ON)
option(CPPBOOT_BUILD_BENCHMARKS "Build the benchmark server and load generator" OFF)
option(CPPBOOT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/CompilerOptions.cmake)
include(cmake/CppBootTargets.cmake)
include(cmake/CppBootCodegen.cmake)

find_package(Threads REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

add_subdirectory(framework/base)
add_subdirectory(framework/platform)
add_subdirectory(framework/config)
add_subdirectory(framework/logging)
add_subdirectory(framework/core)
add_subdirectory(framework/di)
add_subdirectory(framework/boot)
add_subdirectory(framework/web)
add_subdirectory(framework/data)
add_subdirectory(framework/security)
add_subdirectory(tools/cli)

if(CPPBOOT_BUILD_CODEGEN)
    add_subdirectory(tools/codegen)
endif()

cppboot_install_package()

if(CPPBOOT_BUILD_EXAMPLES)
    add_subdirectory(examples/hello-world)
    add_subdirectory(examples/rest-api)
endif()

if(CPPBOOT_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(CPPBOOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
