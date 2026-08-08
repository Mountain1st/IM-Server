#include "eventloop.hpp"
#include "eventloop_threadpool.hpp"
#include "session.hpp"
#include "tcpserver.hpp"
#include "threadpool.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

namespace {
im::EventLoop* g_loop = nullptr;

void onSignal(int) {
    if (g_loop) g_loop->stop();
}
} // namespace

// 用法: imserver [端口] [IO线程数] [工作线程数]
// 缺省: 端口 9191，IO 线程 3，工作线程 5
int main(int argc, char* argv[]) {
    unsigned short port = 9191;
    unsigned ioThreads = 3;
    unsigned workThreads = 5;

    auto parseUint = [](const char* s) -> unsigned {
        try {
            return static_cast<unsigned>(std::stoul(s));
        } catch (...) {
            return 0;
        }
    };

    if (argc > 1) {
        unsigned p = parseUint(argv[1]);
        port = (p > 0 && p < 65536) ? static_cast<unsigned short>(p) : 9191;
    }
    if (argc > 2) {
        unsigned v = parseUint(argv[2]);
        if (v > 0 && v <= 64) ioThreads = v;
    }
    if (argc > 3) {
        unsigned v = parseUint(argv[3]);
        if (v > 0 && v <= 256) workThreads = v;
    }

    // 主线程：accept 事件的 reactor（监听 loop）
    im::EventLoop acceptorLoop;
    g_loop = &acceptorLoop;

    // IO 线程池：每个线程一个 EventLoop，负责已建立连接的 ET 读写
    im::EventLoopThreadPool ioPool(ioThreads);
    ioPool.start();

    // 工作线程池：帧解析与业务逻辑
    im::ThreadPool workPool(workThreads);
    std::cout << "[server] 配置: 端口 " << port
              << ", IO 线程 " << ioThreads
              << ", 工作线程 " << workThreads << std::endl;

    auto server = std::make_shared<im::TcpServer>(&acceptorLoop, port, &ioPool);
    server->setNewConnectionCallback(
        [&](im::EventLoop* ioLoop, const im::TcpServer::ConnectionPtr& conn) {
            auto session = std::make_shared<im::Session>(ioLoop, &workPool, conn);
            session->setClosedHandler([server, conn] {
                server->removeConnection(conn);
            });
            // 在连接所属 IO loop 线程上启动
            ioLoop->runInLoop([session] { session->start(); });
        });
    server->start();

    struct sigaction sa{};
    sa.sa_handler = &onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    std::cout << "[server] 启动完成，按 Ctrl+C 退出" << std::endl;
    acceptorLoop.run();
    std::cout << "[server] 已退出" << std::endl;
    return 0;
}