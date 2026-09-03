cmake_minimum_required(VERSION 3.20)

project(hp_http_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hp_http_core STATIC
    src/base/logger.cpp
    src/http/http_request.cpp
    src/http/http_response.cpp
    src/http/static_file_service.cpp
    src/net/epoller.cpp
    src/net/socket.cpp
    src/net/tcp_server.cpp
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

    add_executable(network_primitives_tests tests/network_primitives_tests.cpp)
    target_link_libraries(network_primitives_tests PRIVATE hp_http_core)

    add_executable(connection_io_tests tests/connection_io_tests.cpp)
    target_link_libraries(connection_io_tests PRIVATE hp_http_core)

    add_executable(http_parser_tests tests/http_parser_tests.cpp)
    target_link_libraries(http_parser_tests PRIVATE hp_http_core)

    add_executable(static_file_tests tests/static_file_tests.cpp)
    target_link_libraries(static_file_tests PRIVATE hp_http_core)

    add_executable(cli_tests tests/cli_tests.cpp)

    add_executable(http_server_integration_tests
                   tests/http_server_integration_tests.cpp)

    foreach(test_target IN ITEMS base_tests socket_tests
                                 network_primitives_tests connection_io_tests
                                 http_parser_tests static_file_tests cli_tests
                                 http_server_integration_tests)
        enable_debug_warnings(${test_target})
    endforeach()

    add_test(NAME base_tests COMMAND base_tests)
    add_test(NAME socket_tests COMMAND socket_tests)
    add_test(NAME network_primitives_tests COMMAND network_primitives_tests)
    add_test(NAME connection_io_tests COMMAND connection_io_tests)
    add_test(NAME http_parser_tests COMMAND http_parser_tests)
    add_test(NAME static_file_tests COMMAND static_file_tests)
    add_test(NAME cli_tests COMMAND cli_tests $<TARGET_FILE:hp_http_server>)
    add_test(NAME http_server_integration_tests
             COMMAND http_server_integration_tests
                     $<TARGET_FILE:hp_http_server>)
    # Preserve the S2 integration identity while running the stronger S3 suite.
    add_test(NAME server_integration_tests
             COMMAND http_server_integration_tests
                     $<TARGET_FILE:hp_http_server>)
    set_tests_properties(cli_tests http_server_integration_tests
                         server_integration_tests PROPERTIES
        ENVIRONMENT
        "HP_S3_TEST_TMP_ROOT=${CMAKE_SOURCE_DIR}/.cache/olympus-v0.1-s3/tests")
endif()
