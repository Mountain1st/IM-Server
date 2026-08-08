cmake_minimum_required(VERSION 3.15)
project(imserver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

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
)
target_include_directories(imserver PRIVATE src)
target_link_libraries(imserver PRIVATE Threads::Threads)

# 测试客户端（仅测试用）
add_executable(imclient
    client/client.cpp
)
target_include_directories(imclient PRIVATE src)
target_link_libraries(imclient PRIVATE Threads::Threads)