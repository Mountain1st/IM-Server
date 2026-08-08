#include "session.hpp"

#include "connection.hpp"
#include "eventloop.hpp"
#include "threadpool.hpp"

#include <iostream>
#include <sstream>

namespace im {

Session::Session(EventLoop* loop, ThreadPool* pool,
                 const std::shared_ptr<Connection>& conn)
    : loop_(loop), pool_(pool), conn_(conn) {}

Session::~Session() {
    std::cout << "[session] 会话销毁" << std::endl;
}

void Session::start() {
    auto self = shared_from_this();
    conn_->setMessageCallback(
        [self](const auto& conn, const char* data, size_t len) {
            // 读事件在 loop 线程发生，把数据按序入队，由线程池串行处理
            self->enqueueAndDrain(data, len);
        });
    conn_->setCloseCallback([self](const std::shared_ptr<Connection>&) {
        self->onClose(self->conn_);
    });
    conn_->start();
}

void Session::enqueueAndDrain(const char* data, size_t len) {
    bool needTask = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        pendingData_.emplace_back(data, len);
        if (!draining_) {
            draining_ = true;
            needTask = true;
        }
    }
    if (needTask) {
        auto self = shared_from_this();
        pool_->submit([self] { self->drain(); });
    }
}

void Session::drain() {
    for (;;) {
        std::string chunk;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (pendingData_.empty()) {
                draining_ = false;
                return;
            }
            chunk = std::move(pendingData_.front());
            pendingData_.pop_front();
        }
        onMessage(conn_, chunk.data(), chunk.size());
    }
}

void Session::onMessage(const std::shared_ptr<Connection>&, const char* data, size_t len) {
    if (!handshaked_) {
        // 累积 HTTP 握手头
        httpBuffer_.append(data, len);
        size_t end = httpBuffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (httpBuffer_.size() > 8192) {
                std::cerr << "[session] 握手头过大" << std::endl;
                conn_->shutdown();
            }
            return;
        }
        std::string request = httpBuffer_.substr(0, end + 4);
        handleHandshake(request);
        if (!handshaked_) return; // 握手失败已关闭

        std::string rest = httpBuffer_.substr(end + 4);
        httpBuffer_.clear();
        if (!rest.empty()) parser_.feed(rest.data(), rest.size());
        processFrames();
        return;
    }
    parser_.feed(data, len);
    processFrames();
}

void Session::processFrames() {
    ws::Frame frame;
    while (parser_.parseOne(&frame)) {
        handle(frame);
    }
}

void Session::handleHandshake(const std::string& request) {
    std::string key;
    if (!ws::parseHandshakeKey(request, &key)) {
        std::cerr << "[session] 缺 Sec-WebSocket-Key，拒绝握手" << std::endl;
        conn_->shutdown();
        return;
    }
    std::string accept = ws::computeAccept(key);

    std::ostringstream oss;
    oss << "HTTP/1.1 101 Switching Protocols\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Accept: " << accept << "\r\n"
        << "\r\n";
    conn_->send(oss.str());
    handshaked_ = true;
    std::cout << "[session] WebSocket 握手成功" << std::endl;
}

void Session::handle(const ws::Frame& frame) {
    switch (frame.opcode) {
    case ws::Opcode::Text:
    case ws::Opcode::Binary:
        if (frame.fin) {
            handleData(frame.opcode, frame.payload);
        } else {
            fragmenting_ = true;
            fragmentOpcode_ = frame.opcode;
            fragmentBuffer_ = frame.payload;
        }
        break;
    case ws::Opcode::Continuation:
        if (!fragmenting_) {
            std::cerr << "[session] 收到意外分片帧" << std::endl;
            conn_->shutdown();
            return;
        }
        fragmentBuffer_ += frame.payload;
        if (frame.fin) {
            handleData(fragmentOpcode_, fragmentBuffer_);
            fragmenting_ = false;
            fragmentBuffer_.clear();
        }
        break;
    case ws::Opcode::Ping:
        // 自动回 Pong
        conn_->send(ws::buildFrame(ws::Opcode::Pong, frame.payload));
        break;
    case ws::Opcode::Pong:
        break;
    case ws::Opcode::Close:
        // 回 Close 帧并关闭连接
        conn_->send(ws::buildFrame(ws::Opcode::Close, frame.payload));
        conn_->shutdown();
        break;
    default:
        conn_->shutdown();
        break;
    }
}

void Session::handleData(ws::Opcode opcode, const std::string& payload) {
    // 业务入口：目前仅回显，后续在此接入登录/消息路由等 IM 逻辑
    if (opcode == ws::Opcode::Text) {
        std::cout << "[session] 收到文本消息: " << payload << std::endl;
        reply("echo: " + payload);
    }
}

void Session::reply(const std::string& text) {
    conn_->send(ws::buildFrame(ws::Opcode::Text, text));
}

void Session::onClose(const std::shared_ptr<Connection>&) {
    std::cout << "[session] 连接关闭" << std::endl;
    if (closedHandler_) closedHandler_();
}

} // namespace im