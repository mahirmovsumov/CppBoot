add_library(cppboot-web
    src/http.cpp
    src/rate_limit.cpp
    src/router.cpp
    src/server.cpp
    src/server_module.cpp
)
add_library(cppboot::web ALIAS cppboot-web)

target_include_directories(cppboot-web PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cppboot-web
    PUBLIC
        cppboot::base
        cppboot::core
        nlohmann_json::nlohmann_json
    PRIVATE
        Boost::headers
)

if(WIN32)
    target_compile_definitions(cppboot-web PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(cppboot-web PRIVATE ws2_32 mswsock)
endif()

cppboot_apply_compiler_options(cppboot-web)
cppboot_install_library(cppboot-web web)
