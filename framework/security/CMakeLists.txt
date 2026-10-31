add_library(cppboot-security
    src/guard.cpp
    src/password.cpp
    src/token.cpp
)
add_library(cppboot::security ALIAS cppboot-security)

target_include_directories(cppboot-security PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cppboot-security
    PUBLIC
        cppboot::base
        cppboot::config
        cppboot::web
    PRIVATE
        OpenSSL::Crypto
)

cppboot_apply_compiler_options(cppboot-security)
cppboot_install_library(cppboot-security security)
