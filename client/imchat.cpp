// 交互式 WebSocket 客户端：等待用户输入，按下 Enter 后才发送给服务器
// 用法: imchat [主机] [端口]
// 缺省: 主机 127.0.0.1，端口 9191
// 输入 "exit" 或 "quit" 关闭连接退出
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <iostream>
#include <string>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

int main(int argc, char* argv[]) {
  std::string host = "127.0.0.1";
  std::string port = "9191";

  if (argc >= 3) {
    host = argv[1];
    port = argv[2];
  }

  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    auto const results = resolver.resolve(host, port);

    websocket::stream<tcp::socket> ws(ioc);
    net::connect(ws.next_layer(), results.begin(), results.end());

    ws.handshake(host, "/");
    std::cout << "[chat] 已连接到 " << host << ":" << port << "（输入 exit 退出）" << std::endl;

    std::string line;
    while (true) {
      std::cout << "> " << std::flush;
      if (!std::getline(std::cin, line)) break; // EOF

      if (line == "exit" || line == "quit") {
        break;
      }
      if (line.empty()) continue;

      // 用户按下 Enter 后，把输入发给服务器
      std::cout << "[chat] 发送: " << line << std::endl;
      ws.write(net::buffer(line));

      // 读取服务器回复
      beast::flat_buffer buffer;
      ws.read(buffer);
      std::cout << "[chat] 服务器: " << beast::buffers_to_string(buffer.data()) << std::endl;
    }

    try {
      ws.close(websocket::close_code::normal);
      std::cout << "[chat] 已正常关闭连接" << std::endl;
    } catch (boost::system::system_error const& e) {
      std::cout << "[chat] 连接已由服务器关闭: " << e.code().message() << std::endl;
    }
  } catch (std::exception const& e) {
    std::cerr << "[chat] 错误: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}