# Runs cppboot-codegen on a target's headers during the build. The target can then include
# "cppboot_components.generated.hpp", which defines cppboot::generated::registerComponents().
#
#   cppboot_generate_components(my-app HEADERS src/services.hpp src/repositories.hpp)
#
# With ACTUATOR, it also adds the /actuator health and metrics routes, and starts a web server even
# if there are no controllers.
function(cppboot_generate_components target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "ACTUATOR" "" "HEADERS")

    if(NOT TARGET cppboot::codegen)
        message(FATAL_ERROR "This CppBoot installation doesn't include cppboot-codegen.")
    endif()

    set(tool_options "")
    if(ARG_ACTUATOR)
        set(tool_options --actuator)
        target_link_libraries(${target} PRIVATE cppboot::actuator)
    endif()

    set(headers "")
    foreach(header IN LISTS ARG_HEADERS)
        cmake_path(ABSOLUTE_PATH header BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE absolute)
        list(APPEND headers ${absolute})
    endforeach()

    set(output_dir ${CMAKE_CURRENT_BINARY_DIR}/cppboot_generated/${target})
    set(output ${output_dir}/cppboot_components.generated.hpp)

    add_custom_command(
        OUTPUT ${output}
        COMMAND cppboot::codegen --output ${output} ${tool_options} ${headers}
            -- "-I$<JOIN:$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>,;-I>"
        DEPENDS cppboot::codegen ${headers}
        COMMENT "Generating CppBoot components for ${target}"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    target_sources(${target} PRIVATE ${output})
    target_include_directories(${target} PRIVATE ${output_dir})
endfunction()
