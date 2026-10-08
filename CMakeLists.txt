cmake_minimum_required(VERSION 3.20)

project(hp_http_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hp_http_core STATIC
    src/metrics/ServerMetrics.cpp
    src/base/Logger.cpp
    src/base/AsyncLogger.cpp
    src/base/FileRegion.cpp
    src/base/Buffer.cpp
    src/timer/TimerQueue.cpp
    src/http/HttpRequest.cpp
    src/http/HttpResponse.cpp
    src/http/StaticFileService.cpp
    src/net/Acceptor.cpp
    src/net/TcpConnection.cpp
    src/net/ConnectionIo.cpp
    src/net/Channel.cpp
    src/net/ConnectionRegistry.cpp
    src/net/EventLoopThreadPool.cpp
    src/net/EventLoopThread.cpp
    src/net/EventLoop.cpp
    src/net/Epoller.cpp
    src/net/Socket.cpp
    src/net/TcpServer.cpp
)
target_include_directories(hp_http_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(hp_http_core PUBLIC Threads::Threads)

add_library(hp_http_app STATIC app/HttpConnectionHandler.cpp app/AccessLog.cpp app/SignalWatcher.cpp)
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
    # R6 freezes legacy source. API-coupled targets are deliberately unregistered;
    # see README for the exact 23-target compatibility list and coverage limits.
    foreach(test_target IN ITEMS cli_tests http_server_integration_tests http_keep_alive_integration_tests)
        add_executable(${test_target} tests/${test_target}.cpp)
        enable_debug_warnings(${test_target})
        add_test(NAME ${test_target} COMMAND ${test_target} $<TARGET_FILE:hp_http_server>)
        set_tests_properties(${test_target} PROPERTIES TIMEOUT 60
            ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp")
    endforeach()
    add_test(NAME server_integration_tests COMMAND http_server_integration_tests $<TARGET_FILE:hp_http_server>)
    set_tests_properties(server_integration_tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME benchmark_runner_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_runner_tests.py)
    set_tests_properties(benchmark_runner_tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp;PYTHONDONTWRITEBYTECODE=1")
endif()

if(BUILD_TESTING)
    add_executable(async_logger_batch_tests tests/AsyncLoggerBatch_test.cpp)
    target_link_libraries(async_logger_batch_tests PRIVATE hp_http_core)
    enable_debug_warnings(async_logger_batch_tests)
    add_test(NAME async_logger_batch_tests COMMAND async_logger_batch_tests)
    set_tests_properties(async_logger_batch_tests PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING)
    add_executable(r6_callbacks_tests tests/R6Callbacks_test.cpp)
    target_link_libraries(r6_callbacks_tests PRIVATE hp_http_app)
    target_link_options(r6_callbacks_tests PRIVATE -Wl,--wrap=_Znwm -Wl,--wrap=pthread_mutex_unlock)
    enable_debug_warnings(r6_callbacks_tests)
    add_test(NAME r6_callbacks_tests COMMAND r6_callbacks_tests)
    set_tests_properties(r6_callbacks_tests PROPERTIES TIMEOUT 60)
endif()

if(BUILD_TESTING)
    add_executable(tcp_nodelay_tests tests/TcpNoDelay_test.cpp)
    target_link_libraries(tcp_nodelay_tests PRIVATE hp_http_core)
    target_link_options(tcp_nodelay_tests PRIVATE -Wl,--wrap=accept4 -Wl,--wrap=setsockopt -Wl,--wrap=close)
    enable_debug_warnings(tcp_nodelay_tests)
    add_test(NAME tcp_nodelay_tests COMMAND tcp_nodelay_tests)
    set_tests_properties(tcp_nodelay_tests PROPERTIES TIMEOUT 30)
    add_test(NAME tcp_nodelay_http_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/TcpNoDelayHttp_test.py $<TARGET_FILE:hp_http_server>)
    set_tests_properties(tcp_nodelay_http_tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp;PYTHONDONTWRITEBYTECODE=1")
endif()

if(BUILD_TESTING)
    add_executable(server_metrics_tests tests/ServerMetrics_test.cpp)
    target_link_libraries(server_metrics_tests PRIVATE hp_http_core)
    enable_debug_warnings(server_metrics_tests)
    add_test(NAME server_metrics_tests COMMAND server_metrics_tests)
    add_executable(http_observability_tests tests/HttpObservability_test.cpp)
    target_link_libraries(http_observability_tests PRIVATE hp_http_app)
    target_link_options(http_observability_tests PRIVATE
        -Wl,--wrap=_ZN2hp4base4infoESt17basic_string_viewIcSt11char_traitsIcEE -Wl,--wrap=_Znwm)
    enable_debug_warnings(http_observability_tests)
    add_test(NAME http_observability_tests COMMAND http_observability_tests)
    set_tests_properties(server_metrics_tests http_observability_tests PROPERTIES TIMEOUT 30)
    add_test(NAME observability_http_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ObservabilityHttp_test.py $<TARGET_FILE:hp_http_server>)
    set_tests_properties(observability_http_tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "HP_S3_TEST_TMP_ROOT=${CMAKE_BINARY_DIR}/test-tmp;PYTHONDONTWRITEBYTECODE=1")
endif()
