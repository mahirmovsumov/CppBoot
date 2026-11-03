# With MinGW, libraries have to come from the compiler's own folder. Other copies, such as the
# PostgreSQL that its Windows installer puts in "C:/Program Files", are built for MSVC and don't link,
# and CMake can find them first. So this makes each search start in the compiler's folder, unless a
# project has already chosen somewhere else.
if(MINGW AND CMAKE_CXX_COMPILER)
    cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH _cppboot_compiler_bin)
    cmake_path(GET _cppboot_compiler_bin PARENT_PATH _cppboot_toolchain_root)
    foreach(_cppboot_root_variable IN ITEMS PostgreSQL_ROOT SQLite3_ROOT OPENSSL_ROOT_DIR)
        if(NOT DEFINED ${_cppboot_root_variable} AND NOT DEFINED ENV{${_cppboot_root_variable}})
            set(${_cppboot_root_variable} "${_cppboot_toolchain_root}")
        endif()
    endforeach()
    unset(_cppboot_root_variable)
    unset(_cppboot_compiler_bin)
    unset(_cppboot_toolchain_root)
endif()
