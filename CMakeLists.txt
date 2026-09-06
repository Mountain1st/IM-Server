cmake_minimum_required(VERSION 3.15)
project(imserver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sanitizer 构建：cmake -DIM_SANITIZE=ON（address,undefined；可选 TSan）
option(IM_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(IM_TSAN "Enable ThreadSanitizer" OFF)
if(IM_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()
if(IM_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# MySQL 用户存储为必需依赖（需 libmysqlclient-dev）
find_path(MYSQL_INCLUDE_DIR NAMES mysql/mysql.h)
find_library(MYSQLCLIENT_LIB NAMES mysqlclient)
if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQLCLIENT_LIB)
    message(FATAL_ERROR "MySQL dev files not found. Install libmysqlclient-dev: sudo apt install libmysqlclient-dev")
endif()

# Redis（token 等存储）：需 libhiredis-dev；缺失时仅编译内存后端
find_path(HIREDIS_INCLUDE_DIR NAMES hiredis/hiredis.h)
find_library(HIREDIS_LIB NAMES hiredis)
if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIB)
    set(HAVE_HIREDIS 1)
    message(STATUS "hiredis found: ${HIREDIS_LIB}")
else()
    message(WARNING "hiredis not found. Redis 存储后端禁用，仅内存后端。Install: sudo apt install libhiredis-dev")
endif()

# 服务器：epoll ET + 线程池 + 自实现 WebSocket
add_executable(imserver
    src/main.cpp
    src/util.cpp
    src/eventloop.cpp
    src/channel.cpp
    src/threadpool.cpp
    src/eventloop_thread.cpp
    src/eventloop_threadpool.cpp
    src/connection.cpp
    src/tcpserver.cpp
    src/websocket.cpp
    src/session.cpp
    src/message.cpp
    src/user_store.cpp
    src/token_store.cpp
    src/mysql_client.cpp
    src/mysql_user_repository.cpp
    src/mysql_message_repository.cpp
    src/contact_repository.cpp
    src/online_manager.cpp
    src/msg_dedup.cpp
    src/rate_limiter_store.cpp
    src/metrics.cpp
    src/appserver.cpp
    src/app_auth.cpp
    src/app_message.cpp
    src/app_contact.cpp
    src/app_settings.cpp
)
target_include_directories(imserver PRIVATE src third_party ${MYSQL_INCLUDE_DIR})
target_link_libraries(imserver PRIVATE Threads::Threads ${MYSQLCLIENT_LIB})
if(HAVE_HIREDIS)
    target_sources(imserver PRIVATE src/redis_client.cpp src/redis_token_store.cpp src/redis_rate_limiter.cpp src/redis_msg_dedup.cpp)
    target_include_directories(imserver PRIVATE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(imserver PRIVATE ${HIREDIS_LIB})
endif()

# 测试客户端（仅测试用）
add_executable(imclient
    client/client.cpp
)
target_include_directories(imclient PRIVATE src)
target_link_libraries(imclient PRIVATE Threads::Threads)

# 交互式客户端：等待用户输入，Enter 后发送
add_executable(imchat
    client/imchat.cpp
)
target_include_directories(imchat PRIVATE src)
target_link_libraries(imchat PRIVATE Threads::Threads)

# ---- 单元测试（P1-3）----
enable_testing()

# 测试目标共用的纯逻辑源码（不依赖网络层/MySQL）
set(UNIT_TEST_SOURCES
    src/util.cpp
    src/websocket.cpp
    src/msg_dedup.cpp
    src/rate_limiter_store.cpp
    src/user_store.cpp
    src/token_store.cpp
)
set(UNIT_TEST_INCLUDES src third_party)

function(im_add_unit_test target test_file)
    add_executable(${target} ${test_file} ${UNIT_TEST_SOURCES})
    target_include_directories(${target} PRIVATE ${UNIT_TEST_INCLUDES})
    target_link_libraries(${target} PRIVATE Threads::Threads)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

im_add_unit_test(ws_parser_test tests/websocket_parser_test.cpp)
im_add_unit_test(ws_handshake_test tests/websocket_handshake_test.cpp)
im_add_unit_test(msg_dedup_test tests/msg_dedup_test.cpp)
im_add_unit_test(rate_limiter_test tests/rate_limiter_test.cpp)
im_add_unit_test(log_test tests/log_test.cpp)
im_add_unit_test(target_parse_test tests/target_parse_test.cpp)