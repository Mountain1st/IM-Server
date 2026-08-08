#pragma once

#include "websocket.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace im {

class EventLoop;
class ThreadPool;
class Connection;

// 一个 WebSocket 会话：负责握手、帧解包、分片重组和业务处理。
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(EventLoop* loop, ThreadPool* pool,
            const std::shared_ptr<Connection>& conn);
    ~Session();

    void start();
    void setClosedHandler(std::function<void()> handler) { closedHandler_ = std::move(handler); }

private:
    void onMessage(const std::shared_ptr<Connection>&,
                   const char* data, size_t len);
    void onClose(const std::shared_ptr<Connection>&);
    void processFrames();
    void handle(const ws::Frame& frame);
    void handleData(ws::Opcode opcode, const std::string& payload);
    void handleHandshake(const std::string& request);
    void reply(const std::string& text);

    // 保证同一会话的消息按序、串行被线程池处理
    std::mutex queueMutex_;
    std::deque<std::string> pendingData_;
    bool draining_ = false;
    void enqueueAndDrain(const char* data, size_t len);
    void drain();

    EventLoop* loop_;
    ThreadPool* pool_;
    std::shared_ptr<Connection> conn_;
    std::function<void()> closedHandler_;

    bool handshaked_ = false;
    std::string httpBuffer_;
    ws::FrameParser parser_;
    // 分片重组缓冲
    bool fragmenting_ = false;
    ws::Opcode fragmentOpcode_ = ws::Opcode::Text;
    std::string fragmentBuffer_;
};

} // namespace im