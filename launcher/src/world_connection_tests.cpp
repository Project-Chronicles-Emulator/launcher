#include "world_connection.hpp"

// Winsock2 must precede Windows networking headers.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace chronicles::launcher {
namespace {

class WinsockTestSession {
public:
  WinsockTestSession() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("Could not initialize Winsock for the world connection test");
    }
  }

  ~WinsockTestSession() {
    WSACleanup();
  }

  WinsockTestSession(const WinsockTestSession &) = delete;
  WinsockTestSession &operator=(const WinsockTestSession &) = delete;
};

class TestSocket {
public:
  explicit TestSocket(SOCKET value = INVALID_SOCKET) : value_(value) {}

  ~TestSocket() {
    if (value_ != INVALID_SOCKET) {
      closesocket(value_);
    }
  }

  TestSocket(const TestSocket &) = delete;
  TestSocket &operator=(const TestSocket &) = delete;

  SOCKET get() const {
    return value_;
  }

private:
  SOCKET value_;
};

TEST(WorldConnection, MatchesOnlyTheGameOwnedSelectedPeer) {
  WinsockTestSession winsock;
  TestSocket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  ASSERT_NE(listener.get(), INVALID_SOCKET);

  sockaddr_in listener_address{};
  listener_address.sin_family = AF_INET;
  listener_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  listener_address.sin_port = 0;
  ASSERT_EQ(bind(listener.get(), reinterpret_cast<sockaddr *>(&listener_address),
                 sizeof(listener_address)),
            0);
  ASSERT_EQ(listen(listener.get(), 1), 0);

  int address_length = sizeof(listener_address);
  ASSERT_EQ(
      getsockname(listener.get(), reinterpret_cast<sockaddr *>(&listener_address), &address_length),
      0);
  const int selected_port = ntohs(listener_address.sin_port);

  TestSocket client(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  ASSERT_NE(client.get(), INVALID_SOCKET);
  ASSERT_EQ(connect(client.get(), reinterpret_cast<sockaddr *>(&listener_address),
                    sizeof(listener_address)),
            0);
  TestSocket accepted(accept(listener.get(), nullptr, nullptr));
  ASSERT_NE(accepted.get(), INVALID_SOCKET);

  sockaddr_in client_address{};
  int client_address_length = sizeof(client_address);
  ASSERT_EQ(getsockname(client.get(), reinterpret_cast<sockaddr *>(&client_address),
                        &client_address_length),
            0);
  const int client_port = ntohs(client_address.sin_port);
  // The accepted socket also belongs to this PID and sees the client's ephemeral port.
  int other_port = 1;
  while (other_port == selected_port || other_port == client_port) {
    ++other_port;
  }

  const WorldConnectionTarget selected_world("127.0.0.1", selected_port);
  const WorldConnectionTarget wrong_port("127.0.0.1", other_port);
  const WorldConnectionTarget wrong_host("192.0.2.1", selected_port);
  bool observed_selected_connection = false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (selected_world.has_connection(GetCurrentProcessId())) {
      observed_selected_connection = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  EXPECT_TRUE(observed_selected_connection);
  EXPECT_FALSE(selected_world.has_connection(0));
  EXPECT_FALSE(wrong_port.has_connection(GetCurrentProcessId()));
  EXPECT_FALSE(wrong_host.has_connection(GetCurrentProcessId()));
  EXPECT_THROW(WorldConnectionTarget("https://world.chronicles.example", selected_port),
               std::invalid_argument);
}

} // namespace
} // namespace chronicles::launcher
