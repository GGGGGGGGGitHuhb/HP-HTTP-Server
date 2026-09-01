cmake_minimum_required(VERSION 3.20)

project(hp_http_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hp_http_core STATIC
    src/base/logger.cpp
    src/net/socket.cpp
)
target_include_directories(hp_http_core PUBLIC include)

add_executable(hp_http_server app/main.cpp)
target_link_libraries(hp_http_server PRIVATE hp_http_core)

function(enable_debug_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Debug>:-Wall>
            $<$<CONFIG:Debug>:-Wextra>
            $<$<CONFIG:Debug>:-Wpedantic>
        )
    endif()
endfunction()

enable_debug_warnings(hp_http_core)
enable_debug_warnings(hp_http_server)

include(CTest)
if(BUILD_TESTING)
    add_executable(base_tests tests/base_tests.cpp)
    target_link_libraries(base_tests PRIVATE hp_http_core)

    add_executable(socket_tests tests/socket_tests.cpp)
    target_link_libraries(socket_tests PRIVATE hp_http_core)

    add_executable(cli_tests tests/cli_tests.cpp)

    foreach(test_target IN ITEMS base_tests socket_tests cli_tests)
        enable_debug_warnings(${test_target})
    endforeach()

    add_test(NAME base_tests COMMAND base_tests)
    add_test(NAME socket_tests COMMAND socket_tests)
    add_test(NAME cli_tests COMMAND cli_tests $<TARGET_FILE:hp_http_server>)
endif()
