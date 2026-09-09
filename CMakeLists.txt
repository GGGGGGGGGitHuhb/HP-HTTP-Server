cmake_minimum_required(VERSION 3.20)

project(hp_http_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hp_http_core STATIC
    src/base/logger.cpp
    src/timer/timer_queue.cpp
    src/http/http_request.cpp
    src/http/http_response.cpp
    src/http/static_file_service.cpp
    src/net/acceptor.cpp
    src/net/tcp_connection.cpp
    src/net/connection_io.cpp
    src/net/channel.cpp
    src/net/connection_registry.cpp
    src/net/event_loop_thread_pool.cpp
    src/net/event_loop_thread.cpp
    src/net/event_loop.cpp
    src/net/epoller.cpp
    src/net/socket.cpp
    src/net/tcp_server.cpp
)
target_include_directories(hp_http_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(hp_http_core PUBLIC Threads::Threads)

add_library(hp_http_app STATIC app/http_connection_handler.cpp app/signal_watcher.cpp)
target_include_directories(hp_http_app PUBLIC app)
target_link_libraries(hp_http_app PUBLIC hp_http_core)

add_executable(hp_http_server app/main.cpp)
target_link_libraries(hp_http_server PRIVATE hp_http_app)

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
enable_debug_warnings(hp_http_app)
enable_debug_warnings(hp_http_server)

include(CTest)
if(BUILD_TESTING)
    add_executable(base_tests tests/base_tests.cpp)
    target_link_libraries(base_tests PRIVATE hp_http_core)

    add_executable(socket_tests tests/socket_tests.cpp)
    target_link_libraries(socket_tests PRIVATE hp_http_core)

    add_executable(network_primitives_tests tests/network_primitives_tests.cpp)
    target_link_libraries(network_primitives_tests PRIVATE hp_http_core)

    add_executable(http_connection_callback_tests tests/http_connection_callback_tests.cpp)
    target_link_libraries(http_connection_callback_tests PRIVATE hp_http_app)

    add_executable(acceptor_tcp_connection_tests tests/acceptor_tcp_connection_tests.cpp)
    target_link_libraries(acceptor_tcp_connection_tests PRIVATE hp_http_core)

    add_executable(event_loop_channel_tests tests/event_loop_channel_tests.cpp)
    target_link_libraries(event_loop_channel_tests PRIVATE hp_http_core)

    add_executable(connection_io_tests tests/connection_io_tests.cpp)
    target_link_libraries(connection_io_tests PRIVATE hp_http_core)

    add_executable(http_parser_state_tests tests/http_parser_state_tests.cpp)
    target_link_libraries(http_parser_state_tests PRIVATE hp_http_core)

    add_executable(http_parser_tests tests/http_parser_tests.cpp)
    target_link_libraries(http_parser_tests PRIVATE hp_http_core)

    add_executable(static_file_tests tests/static_file_tests.cpp)
    target_link_libraries(static_file_tests PRIVATE hp_http_core)

    add_executable(cli_tests tests/cli_tests.cpp)

    add_executable(http_server_integration_tests
                   tests/http_server_integration_tests.cpp)

    foreach(test_target IN ITEMS base_tests socket_tests
                                 network_primitives_tests connection_io_tests
                                 event_loop_channel_tests acceptor_tcp_connection_tests http_connection_callback_tests
                                 http_parser_tests http_parser_state_tests static_file_tests cli_tests
                                 http_server_integration_tests)
        enable_debug_warnings(${test_target})
    endforeach()

    add_test(NAME base_tests COMMAND base_tests)
    add_test(NAME socket_tests COMMAND socket_tests)
    add_test(NAME network_primitives_tests COMMAND network_primitives_tests)
    add_test(NAME http_connection_callback_tests COMMAND http_connection_callback_tests)
    add_test(NAME acceptor_tcp_connection_tests COMMAND acceptor_tcp_connection_tests)
    add_test(NAME event_loop_channel_tests COMMAND event_loop_channel_tests)
    add_test(NAME connection_io_tests COMMAND connection_io_tests)
    add_test(NAME http_parser_state_tests COMMAND http_parser_state_tests)
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
    set_tests_properties(static_file_tests cli_tests http_server_integration_tests
                         server_integration_tests PROPERTIES
        ENVIRONMENT
        "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp")
endif()

if(BUILD_TESTING)
    add_executable(http_keep_alive_tests tests/http_keep_alive_tests.cpp)
    target_link_libraries(http_keep_alive_tests PRIVATE hp_http_app)
    enable_debug_warnings(http_keep_alive_tests)
    add_executable(http_keep_alive_integration_tests tests/http_keep_alive_integration_tests.cpp)
    enable_debug_warnings(http_keep_alive_integration_tests)
    add_test(NAME http_keep_alive_tests COMMAND http_keep_alive_tests)
    add_test(NAME http_keep_alive_integration_tests COMMAND http_keep_alive_integration_tests $<TARGET_FILE:hp_http_server>)
    set_tests_properties(http_keep_alive_tests http_keep_alive_integration_tests PROPERTIES
        TIMEOUT 60 ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp")
endif()

if(BUILD_TESTING)
    add_executable(event_loop_thread_tests tests/event_loop_thread_tests.cpp)
    target_link_libraries(event_loop_thread_tests PRIVATE hp_http_core)
    target_link_options(event_loop_thread_tests PRIVATE
        -Wl,--wrap=eventfd -Wl,--wrap=epoll_ctl -Wl,--wrap=epoll_wait
        -Wl,--wrap=read -Wl,--wrap=write)
    enable_debug_warnings(event_loop_thread_tests)
    add_test(NAME event_loop_thread_tests COMMAND event_loop_thread_tests)
    set_tests_properties(event_loop_thread_tests PROPERTIES TIMEOUT 60)
endif()

if(BUILD_TESTING)
    add_executable(event_loop_thread_pool_tests tests/event_loop_thread_pool_tests.cpp)
    target_link_libraries(event_loop_thread_pool_tests PRIVATE hp_http_core)
    enable_debug_warnings(event_loop_thread_pool_tests)
    add_test(NAME event_loop_thread_pool_tests COMMAND event_loop_thread_pool_tests)
    set_tests_properties(event_loop_thread_pool_tests PROPERTIES TIMEOUT 60)
endif()

if(BUILD_TESTING)
    add_executable(multi_reactor_tests tests/multi_reactor_tests.cpp)
    target_link_libraries(multi_reactor_tests PRIVATE hp_http_app)
    target_link_options(multi_reactor_tests PRIVATE -Wl,--wrap=accept4 -Wl,--wrap=close
        -Wl,--wrap=epoll_ctl -Wl,--wrap=_Znwm)
    enable_debug_warnings(multi_reactor_tests)
    add_test(NAME multi_reactor_tests COMMAND multi_reactor_tests $<TARGET_FILE:hp_http_server>)
    set_tests_properties(multi_reactor_tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp")
endif()

if(BUILD_TESTING)
    add_executable(timer_queue_tests tests/timer_queue_tests.cpp)
    target_link_libraries(timer_queue_tests PRIVATE hp_http_core)
    target_link_options(timer_queue_tests PRIVATE -Wl,--wrap=_Znwm -Wl,--wrap=epoll_wait)
    enable_debug_warnings(timer_queue_tests)
    add_test(NAME timer_queue_tests COMMAND timer_queue_tests)
    set_tests_properties(timer_queue_tests PROPERTIES TIMEOUT 60)
endif()

if(BUILD_TESTING)
    add_executable(connection_timeout_tests tests/connection_timeout_tests.cpp)
    target_link_libraries(connection_timeout_tests PRIVATE hp_http_app)
    target_link_options(connection_timeout_tests PRIVATE -Wl,--wrap=accept4 -Wl,--wrap=close
        -Wl,--wrap=epoll_ctl -Wl,--wrap=_Znwm)
    enable_debug_warnings(connection_timeout_tests)
    add_test(NAME connection_timeout_tests COMMAND connection_timeout_tests)
    set_tests_properties(connection_timeout_tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp")
endif()

if(BUILD_TESTING)
    add_executable(resource_limits_tests tests/resource_limits_tests.cpp)
    target_link_libraries(resource_limits_tests PRIVATE hp_http_app)
    target_link_options(resource_limits_tests PRIVATE -Wl,--wrap=accept4 -Wl,--wrap=close
        -Wl,--wrap=epoll_ctl -Wl,--wrap=_Znwm)
    enable_debug_warnings(resource_limits_tests)
    add_test(NAME resource_limits_tests COMMAND resource_limits_tests)
    set_tests_properties(resource_limits_tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp")
endif()

if(BUILD_TESTING)
    add_executable(graceful_shutdown_tests tests/graceful_shutdown_tests.cpp)
    target_link_libraries(graceful_shutdown_tests PRIVATE hp_http_app)
    target_link_options(graceful_shutdown_tests PRIVATE -Wl,--wrap=accept4 -Wl,--wrap=close
        -Wl,--wrap=epoll_ctl -Wl,--wrap=_Znwm)
    enable_debug_warnings(graceful_shutdown_tests)
    add_test(NAME graceful_shutdown_tests COMMAND graceful_shutdown_tests $<TARGET_FILE:hp_http_server>)
    set_tests_properties(graceful_shutdown_tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp")
endif()
