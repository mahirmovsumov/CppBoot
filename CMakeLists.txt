cmake_minimum_required(VERSION 3.25)

project(CppBoot VERSION 0.1.0 DESCRIPTION "Native application framework for C++20" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CPPBOOT_BUILD_TESTS "Build CppBoot tests" ON)
option(CPPBOOT_BUILD_EXAMPLES "Build CppBoot examples" ON)
option(CPPBOOT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/CompilerOptions.cmake)

find_package(yaml-cpp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_subdirectory(framework/base)
add_subdirectory(framework/platform)
add_subdirectory(framework/config)
add_subdirectory(framework/logging)
add_subdirectory(framework/core)

if(CPPBOOT_BUILD_EXAMPLES)
    add_subdirectory(examples/hello-world)
endif()

if(CPPBOOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
