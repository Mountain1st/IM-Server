cmake_minimum_required(VERSION 3.15)
project(imserver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 代码规范：警告全开（零警告基线，新代码不得引入警告）
add_compile_options(-Wall -Wextra)
# 三方头（nlohmann/json.hpp）警告不干净，隔离
add_compile_options(-Wno-unused-parameter)

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

# libcurl（对象存储 HEAD/DELETE）：
# 优先系统 find_package(CURL)；无系统 dev 包时回退读环境变量 IM_CURL_ROOT
# （期望前缀下含 usr/include/x86_64-linux-gnu 与 usr/lib/x86_64-linux-gnu），
# 统一封装为导入目标 CURL::libcurl。两种方式均失败 → FATAL_ERROR。
find_package(CURL QUIET)
if(CURL_FOUND)
    message(STATUS "libcurl found via find_package(CURL): ${CURL_LIBRARIES}")
else()
    # 兼容 -DIM_CURL_ROOT=... 与环境变量 IM_CURL_ROOT
    if(NOT IM_CURL_ROOT AND DEFINED ENV{IM_CURL_ROOT})
        set(IM_CURL_ROOT "$ENV{IM_CURL_ROOT}")
    endif()
    if(NOT IM_CURL_ROOT)
        message(FATAL_ERROR
            "libcurl not found. 安装 libcurl4-openssl-dev，"
            "或设置环境变量 IM_CURL_ROOT 指向 libcurl 安装前缀"
            "（如 /path/to/curlroot，其下含 usr/include/x86_64-linux-gnu 与 usr/lib/x86_64-linux-gnu）")
    endif()
    find_path(IM_CURL_INCLUDE_DIR NAMES curl/curl.h
        PATHS
            "${IM_CURL_ROOT}/usr/include/x86_64-linux-gnu"
            "${IM_CURL_ROOT}/usr/include"
            "${IM_CURL_ROOT}/include/x86_64-linux-gnu"
            "${IM_CURL_ROOT}/include"
        NO_DEFAULT_PATH)
    find_library(IM_CURL_LIBRARY NAMES curl libcurl
        PATHS
            "${IM_CURL_ROOT}/usr/lib/x86_64-linux-gnu"
            "${IM_CURL_ROOT}/usr/lib"
            "${IM_CURL_ROOT}/lib/x86_64-linux-gnu"
            "${IM_CURL_ROOT}/lib"
        NO_DEFAULT_PATH)
    if(NOT IM_CURL_INCLUDE_DIR OR NOT IM_CURL_LIBRARY)
        message(FATAL_ERROR
            "libcurl not found under IM_CURL_ROOT=${IM_CURL_ROOT}. "
            "请确认该前缀下存在 include/.../curl/curl.h 与 lib/.../libcurl.so")
    endif()
    message(STATUS "libcurl found via IM_CURL_ROOT: ${IM_CURL_LIBRARY}")
    add_library(CURL::libcurl UNKNOWN IMPORTED)
    set_target_properties(CURL::libcurl PROPERTIES
        IMPORTED_LOCATION "${IM_CURL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${IM_CURL_INCLUDE_DIR}")
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
    src/mysql_media_repository.cpp
    src/contact_repository.cpp
    src/block_repository.cpp
    src/online_manager.cpp
    src/msg_dedup.cpp
    src/rate_limiter_store.cpp
    src/metrics.cpp
    src/appserver.cpp
    src/media_store.cpp
    src/app_auth.cpp
    src/app_message.cpp
    src/app_contact.cpp
    src/group_repository.cpp
    src/app_block.cpp
    src/app_group.cpp
    src/app_settings.cpp
    src/sigv4.cpp
    src/minio_object_store.cpp
    src/media_service.cpp
)
target_include_directories(imserver PRIVATE src third_party ${MYSQL_INCLUDE_DIR})
target_link_libraries(imserver PRIVATE Threads::Threads ${MYSQLCLIENT_LIB} CURL::libcurl)
if(HAVE_HIREDIS)
    target_sources(imserver PRIVATE src/redis_client.cpp src/redis_token_store.cpp src/redis_rate_limiter.cpp src/redis_msg_dedup.cpp src/redis_presence_store.cpp)
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
    src/sigv4.cpp
    src/media_store.cpp
    src/websocket.cpp
    src/msg_dedup.cpp
    src/rate_limiter_store.cpp
    src/user_store.cpp
    src/token_store.cpp
    src/threadpool.cpp
)
set(UNIT_TEST_INCLUDES src third_party)

function(im_add_unit_test target test_file)
    add_executable(${target} ${test_file} ${ARGN} ${UNIT_TEST_SOURCES})
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
im_add_unit_test(login_guard_test tests/login_guard_test.cpp)
im_add_unit_test(tcpserver_test tests/tcpserver_test.cpp)
im_add_unit_test(threadpool_test tests/threadpool_test.cpp)
im_add_unit_test(media_store_test tests/test_media_store.cpp)
# MediaService 直连测试：fake 仓储/对象存储（media_service.cpp 依赖 g_metrics）
im_add_unit_test(media_service_test tests/media_service_test.cpp src/media_service.cpp src/metrics.cpp)
im_add_unit_test(crypto_test tests/crypto_test.cpp)
im_add_unit_test(sigv4_test tests/sigv4_test.cpp)
im_add_unit_test(minio_object_store_test tests/minio_object_store_test.cpp src/minio_object_store.cpp)
# 仅 MinIO 存储测试直接依赖 libcurl（其余单测不链，避免无 curl 环境整体不可构建）
target_link_libraries(minio_object_store_test PRIVATE CURL::libcurl)