# Runs cppboot-codegen on a target's headers during the build. The target can then include
# "cppboot_components.generated.hpp", which defines cppboot::generated::registerComponents().
#
#   cppboot_generate_components(my-app HEADERS src/services.hpp src/repositories.hpp)
#
# With ACTUATOR, it also adds the /actuator health and metrics routes, and starts a web server even
# if there are no controllers.
#
# Each header is read in its own step, with the target's include folders, definitions and options,
# so that it's read as the compiler will read it. A step runs again only when its header, or a file
# that header includes, has changed. A last step checks what all of them found and writes the
# generated file.
function(cppboot_generate_components target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "ACTUATOR" "" "HEADERS")

    if(NOT TARGET cppboot::codegen)
        message(FATAL_ERROR "This CppBoot installation doesn't include cppboot-codegen.")
    endif()

    # The OpenAPI document is named after the target, with the project's version if it has one.
    set(api_version "${PROJECT_VERSION}")
    if(NOT api_version)
        set(api_version "0.0.0")
    endif()
    set(tool_options --title ${target} --version ${api_version})
    if(ARG_ACTUATOR)
        list(APPEND tool_options --actuator)
        target_link_libraries(${target} PRIVATE cppboot::actuator)
    endif()

    # These include what the target gets from the libraries it links to. The tool keeps only the
    # arguments that change what a header declares.
    set(includes "$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>")
    set(definitions "$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>")
    # $<SEMICOLON> keeps each list whole until the command is built, where it splits into arguments.
    set(compiler_arguments
        "$<$<BOOL:${includes}>:-I$<JOIN:${includes},$<SEMICOLON>-I>>"
        "$<$<BOOL:${definitions}>:-D$<JOIN:${definitions},$<SEMICOLON>-D>>"
        "$<TARGET_PROPERTY:${target},COMPILE_OPTIONS>"
    )
    separate_arguments(global_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
    list(APPEND compiler_arguments ${global_flags})
    foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} config_upper)
        separate_arguments(config_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS_${config_upper}}")
        if(config_flags)
            list(JOIN config_flags "$<SEMICOLON>" joined)
            list(APPEND compiler_arguments "$<$<CONFIG:${config}>:${joined}>")
        endif()
    endforeach()

    # Make and Ninja can follow what each header includes. Other generators only see the header.
    set(follow_includes OFF)
    if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        set(follow_includes ON)
    endif()

    set(output_dir ${CMAKE_CURRENT_BINARY_DIR}/cppboot_generated/${target})
    set(output ${output_dir}/cppboot_components.generated.hpp)

    set(scans "")
    foreach(header IN LISTS ARG_HEADERS)
        cmake_path(ABSOLUTE_PATH header BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                   OUTPUT_VARIABLE absolute)
        cmake_path(GET absolute FILENAME name)
        # Two headers can share a name in different folders.
        string(MD5 path_hash "${absolute}")
        string(SUBSTRING "${path_hash}" 0 10 path_hash)
        set(scan ${output_dir}/scans/${name}.${path_hash}.json)

        set(depfile_options "")
        set(depfile_arguments "")
        if(follow_includes)
            set(depfile_options DEPFILE ${scan}.d)
            set(depfile_arguments --depfile ${scan}.d)
        endif()

        add_custom_command(
            OUTPUT ${scan}
            COMMAND cppboot::codegen --scan ${absolute} --output ${scan} ${depfile_arguments}
                -- ${compiler_arguments}
            DEPENDS cppboot::codegen ${absolute}
            ${depfile_options}
            COMMENT "Reading CppBoot marks in ${name}"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        list(APPEND scans ${scan})
    endforeach()

    add_custom_command(
        OUTPUT ${output}
        BYPRODUCTS ${output_dir}/openapi.json
        COMMAND cppboot::codegen --merge --output ${output} ${tool_options} ${scans}
        DEPENDS cppboot::codegen ${scans}
        COMMENT "Generating CppBoot components for ${target}"
        VERBATIM
    )

    target_sources(${target} PRIVATE ${output})
    target_include_directories(${target} PRIVATE ${output_dir})
endfunction()
