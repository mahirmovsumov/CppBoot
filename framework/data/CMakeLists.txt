add_library(cppboot-data
    src/database.cpp
    src/database_module.cpp
    src/migrations.cpp
    src/pool.cpp
    src/postgresql.cpp
    src/row.cpp
    src/sqlite.cpp
)
add_library(cppboot::data ALIAS cppboot-data)

target_include_directories(cppboot-data PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
# CMake renamed this target in newer versions, and older ones only have the old name.
if(TARGET SQLite3::SQLite3)
    set(sqlite_target SQLite3::SQLite3)
else()
    set(sqlite_target SQLite::SQLite3)
endif()

target_link_libraries(cppboot-data
    PUBLIC
        cppboot::base
        cppboot::core
    PRIVATE
        PostgreSQL::PostgreSQL
        ${sqlite_target}
)

cppboot_apply_compiler_options(cppboot-data)
cppboot_install_library(cppboot-data data)
