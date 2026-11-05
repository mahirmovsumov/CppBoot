# Drives cppboot dev, which reads what this writes. Run with:
#   cmake -DPROJECT_DIR=... -DEXECUTABLE=... -P <this file>
# It waits for the first build, changes a header, waits for the build that change starts, then tells
# cppboot dev to quit. Messages go to standard error, because standard output is cppboot dev's input.

function(wait_for description condition_script)
    foreach(attempt RANGE 600)
        cmake_language(EVAL CODE "${condition_script}")
        if(done)
            return()
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
    endforeach()
    message(FATAL_ERROR "dev driver: timed out waiting for ${description}")
endfunction()

wait_for("the first build" [=[
    set(done FALSE)
    if(EXISTS "${EXECUTABLE}")
        set(done TRUE)
    endif()
]=])
message(STATUS "dev driver: the first build is done")

# Long enough for cppboot dev to start the application and record what it watches. The time is read
# after this, because the file appears when linking starts and changes again when it ends.
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 5)
file(TIMESTAMP "${EXECUTABLE}" first_build "%s")
file(APPEND ${PROJECT_DIR}/src/greeting.hpp "\n// Changed while cppboot dev runs.\n")

wait_for("the build after a change" [=[
    set(done FALSE)
    file(TIMESTAMP "${EXECUTABLE}" now "%s")
    if(NOT now STREQUAL first_build)
        set(done TRUE)
    endif()
]=])
message(STATUS "dev driver: it built again")

execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 5)
execute_process(COMMAND ${CMAKE_COMMAND} -E echo q)
