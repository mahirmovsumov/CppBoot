include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# A fixed location, so the CLI can find the package on distributions that use lib64.
set(CPPBOOT_INSTALL_CMAKEDIR "lib/cmake/CppBoot")

# Installs a framework library and its headers as cppboot::<export_name>.
function(cppboot_install_library target export_name)
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})

    install(TARGETS ${target}
        EXPORT CppBootTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp"
    )
endfunction()

function(cppboot_install_package)
    install(EXPORT CppBootTargets
        NAMESPACE cppboot::
        DESTINATION ${CPPBOOT_INSTALL_CMAKEDIR}
    )

    configure_package_config_file(
        ${PROJECT_SOURCE_DIR}/cmake/CppBootConfig.cmake.in
        ${PROJECT_BINARY_DIR}/CppBootConfig.cmake
        INSTALL_DESTINATION ${CPPBOOT_INSTALL_CMAKEDIR}
    )
    write_basic_package_version_file(
        ${PROJECT_BINARY_DIR}/CppBootConfigVersion.cmake
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
        ${PROJECT_BINARY_DIR}/CppBootConfig.cmake
        ${PROJECT_BINARY_DIR}/CppBootConfigVersion.cmake
        DESTINATION ${CPPBOOT_INSTALL_CMAKEDIR}
    )
endfunction()
