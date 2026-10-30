add_library(cppboot-platform)
add_library(cppboot::platform ALIAS cppboot-platform)

if(WIN32)
    target_sources(cppboot-platform PRIVATE
        windows/environment.cpp
        windows/signals.cpp
    )
elseif(UNIX)
    target_sources(cppboot-platform PRIVATE
        linux/environment.cpp
        linux/signals.cpp
    )
else()
    message(FATAL_ERROR "CppBoot has no platform implementation for ${CMAKE_SYSTEM_NAME}")
endif()

target_include_directories(cppboot-platform PUBLIC include)
target_link_libraries(cppboot-platform PUBLIC cppboot::base)

cppboot_apply_compiler_options(cppboot-platform)
