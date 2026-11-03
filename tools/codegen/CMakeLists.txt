# Debian and Ubuntu keep each LLVM version in its own folder. Search the newest first, and take the
# library from the same version as the header.
file(GLOB llvm_roots LIST_DIRECTORIES true /usr/lib/llvm-*)
list(SORT llvm_roots COMPARE NATURAL ORDER DESCENDING)

find_path(CPPBOOT_LIBCLANG_INCLUDE_DIR clang-c/Index.h
    HINTS ${llvm_roots}
    PATH_SUFFIXES include
)
if(CPPBOOT_LIBCLANG_INCLUDE_DIR)
    cmake_path(GET CPPBOOT_LIBCLANG_INCLUDE_DIR PARENT_PATH libclang_root)
endif()
find_library(CPPBOOT_LIBCLANG_LIBRARY
    NAMES clang libclang
    HINTS ${libclang_root}/lib
)

if(NOT CPPBOOT_LIBCLANG_INCLUDE_DIR OR NOT CPPBOOT_LIBCLANG_LIBRARY)
    message(FATAL_ERROR
        "libclang wasn't found. Install it, or configure with -DCPPBOOT_BUILD_CODEGEN=OFF.")
endif()

# libclang needs its built-in headers, such as stddef.h, but can't always find them when it's
# loaded as a library. Pass the folder explicitly, from the same version as the library.
file(GLOB resource_dirs LIST_DIRECTORIES true ${libclang_root}/lib/clang/*)
list(SORT resource_dirs COMPARE NATURAL ORDER DESCENDING)
set(libclang_resource_dir "")
foreach(candidate IN LISTS resource_dirs)
    if(EXISTS ${candidate}/include/stddef.h)
        set(libclang_resource_dir ${candidate})
        break()
    endif()
endforeach()
if(NOT libclang_resource_dir)
    message(FATAL_ERROR "Clang's built-in headers weren't found in ${libclang_root}/lib/clang.")
endif()

add_library(cppboot-codegen-lib STATIC
    src/model.cpp
    src/parser.cpp
    src/scan.cpp
)
target_include_directories(cppboot-codegen-lib PUBLIC include)
target_include_directories(cppboot-codegen-lib SYSTEM PRIVATE ${CPPBOOT_LIBCLANG_INCLUDE_DIR})
target_compile_definitions(cppboot-codegen-lib PRIVATE
    CPPBOOT_LIBCLANG_RESOURCE_DIR="${libclang_resource_dir}"
)
target_link_libraries(cppboot-codegen-lib
    PUBLIC cppboot::base
    PRIVATE
        cppboot::platform
        nlohmann_json::nlohmann_json
        ${CPPBOOT_LIBCLANG_LIBRARY}
)
cppboot_apply_compiler_options(cppboot-codegen-lib)

add_executable(cppboot-codegen src/main.cpp)
add_executable(cppboot::codegen ALIAS cppboot-codegen)
set_target_properties(cppboot-codegen PROPERTIES EXPORT_NAME codegen)
target_link_libraries(cppboot-codegen PRIVATE cppboot-codegen-lib)
cppboot_apply_compiler_options(cppboot-codegen)

# The installed tool finds this copy relative to itself, so it doesn't rely on a build-time path.
install(TARGETS cppboot-codegen
    EXPORT CppBootTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${libclang_resource_dir}/include DESTINATION lib/cppboot/clang)
