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
option(CPPBOOT_BUNDLE_RUNTIME "Install the shared libraries that the SDK's own programs need, for a downloadable SDK on Windows" OFF)

include(cmake/CompilerOptions.cmake)
include(cmake/CppBootPreferToolchain.cmake)
include(cmake/CppBootTargets.cmake)
include(cmake/CppBootCodegen.cmake)

find_package(Threads REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
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
add_subdirectory(framework/actuator)
add_subdirectory(tools/cli)

if(CPPBOOT_BUILD_CODEGEN)
    add_subdirectory(tools/codegen)
endif()

cppboot_install_package()
install(FILES LICENSE NOTICE THIRD_PARTY.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/cppboot)

# cppboot doctor compares this with the compiler a project uses, because libraries built by one
# compiler version don't always link with another.
configure_file(cmake/build-info.txt.in ${CMAKE_CURRENT_BINARY_DIR}/build-info.txt @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/build-info.txt DESTINATION ${CMAKE_INSTALL_LIBDIR}/cppboot)

# cppboot and cppboot-codegen have to start before anything is on PATH, such as to run doctor on a
# new machine, so a downloadable SDK carries their libraries. Applications don't use these copies.
if(CPPBOOT_BUNDLE_RUNTIME)
    set(bundled_programs "$<TARGET_FILE:cppboot-cli>")
    if(CPPBOOT_BUILD_CODEGEN)
        list(APPEND bundled_programs "$<TARGET_FILE:cppboot-codegen>")
    endif()
    install(CODE "set(bundled_programs \"${bundled_programs}\")")
    install(CODE [==[
        if(WIN32)
            set(search_directories $ENV{PATH})
        else()
            set(search_directories "")
        endif()
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES ${bundled_programs}
            RESOLVED_DEPENDENCIES_VAR resolved
            UNRESOLVED_DEPENDENCIES_VAR unresolved
            DIRECTORIES ${search_directories}
            PRE_EXCLUDE_REGEXES
                [[^ld-linux.*]] [[^libc\.so.*]] [[^libm\.so.*]] [[^libdl\.so.*]] [[^libpthread\.so.*]]
                [[^librt\.so.*]] [[^api-ms-.*]] [[^ext-ms-.*]]
            POST_EXCLUDE_REGEXES [[.*[/\\][Ss]ystem32[/\\].*]] [[.*[/\\][Ww]indows[/\\].*]]
        )
        if(unresolved)
            message(FATAL_ERROR "Can't find these libraries for the SDK: ${unresolved}")
        endif()
        file(INSTALL ${resolved} DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" FOLLOW_SYMLINK_CHAIN)
    ]==])
endif()

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
