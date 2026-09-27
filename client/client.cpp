// 简单的 C++ WebSocket 客户端，用于验证服务器连接
// 用法: imclient [主机] [端口] [报文内容]
// 缺省: 主机 127.0.0.1，端口 9191，报文 "hello imserver"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <iostream>
#include <string>
#include <unistd.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

int main(int argc, char* argv[]) {
  std::string host = "127.0.0.1";
  std::string port = "9191";
  std::string msg = "hello imserver";

  if (argc >= 3) {
    host = argv[1];
    port = argv[2];
  }
  if (argc >= 4) {
    msg = argv[3];
  }

  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    auto const results = resolver.resolve(host, port);

    websocket::stream<tcp::socket> ws(ioc);
    net::connect(ws.next_layer(), results.begin(), results.end());

    ws.handshake(host, "/");
    std::cout << "[client] WebSocket 握手成功，已连接到 " << host << ":" << port << std::endl;

    // 每次连接后发送一条报文
    std::cout << "[client] 发送报文: " << msg << std::endl;
    ws.write(net::buffer(msg));

    // 读取服务器回显
    sleep(1); // 等 1 秒，待服务器回复返回
    beast::flat_buffer buffer;
    ws.read(buffer);
    std::cout << "[client] 服务器回复: " << beast::buffers_to_string(buffer.data()) << std::endl;

    // 关闭连接（服务器收到 Close 后可能立即关闭，close 阶段的异常可忽略）
    try {
      ws.close(websocket::close_code::normal);
      std::cout << "[client] 已正常关闭连接" << std::endl;
    } catch (boost::system::system_error const& e) {
      std::cout << "[client] 连接已由服务器关闭: " << e.code().message() << std::endl;
    }
  } catch (std::exception const& e) {
    std::cerr << "[client] 错误: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}