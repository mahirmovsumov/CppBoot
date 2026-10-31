add_library(cppboot-platform)
add_library(cppboot::platform ALIAS cppboot-platform)

if(WIN32)
    target_sources(cppboot-platform PRIVATE
        windows/environment.cpp
        windows/process.cpp
        windows/service_manager.cpp
        windows/signals.cpp
        windows/system_info.cpp
    )
    target_link_libraries(cppboot-platform PRIVATE psapi)
elseif(UNIX)
    target_sources(cppboot-platform PRIVATE
        linux/environment.cpp
        linux/process.cpp
        linux/service_manager.cpp
        linux/signals.cpp
        linux/system_info.cpp
    )
else()
    message(FATAL_ERROR "CppBoot has no platform implementation for ${CMAKE_SYSTEM_NAME}")
endif()

target_include_directories(cppboot-platform PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cppboot-platform PUBLIC cppboot::base)

cppboot_apply_compiler_options(cppboot-platform)
cppboot_install_library(cppboot-platform platform)
