#include "discord_loopback.hpp"

#include "distribution/distribution.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace chronicles::launcher {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using Tcp = asio::ip::tcp;

struct DiscordLoopback::Impl {
  asio::io_context context;
  Tcp::acceptor acceptor{context};
  beast::tcp_stream stream{context};
  beast::flat_buffer buffer;
  std::unique_ptr<http::request_parser<http::empty_body>> parser;
  http::response<http::string_body> response;
  std::string state;
  std::string proof;
  bool stopping{};

  Impl() {
    acceptor.open(Tcp::v4());
    // Bind before opening the browser. No public interface or inbound firewall
    // rule is involved, and no other process may reuse this listener's endpoint.
#ifdef _WIN32
    const BOOL exclusive = TRUE;
    if (setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) != 0) {
      throw std::runtime_error("Cannot reserve the Discord browser callback port");
    }
#endif
    acceptor.bind(Tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    acceptor.listen(1);
    accept();
  }

  ~Impl() {
    stopping = true;
    beast::error_code ignored;
    acceptor.close(ignored);
    stream.close();
    // Drain cancellation completions while their parser and response still live.
    context.restart();
    context.run();
  }

  void accept() {
    beast::error_code ignored;
    stream.socket().close(ignored);
    stream.expires_never();
    buffer.consume(buffer.size());
    parser = std::make_unique<http::request_parser<http::empty_body>>();
    parser->header_limit(4096);
    parser->body_limit(0);
    acceptor.async_accept(stream.socket(), [this](beast::error_code error) {
      if (error || stopping) {
        return;
      }
      stream.expires_after(std::chrono::seconds(3));
      http::async_read(stream, buffer, *parser, [this](beast::error_code read_error, std::size_t) {
        if (stopping) {
          return;
        }
        if (read_error) {
          accept();
          return;
        }
        const auto &request = parser->get();
        const auto target = std::string(request.target());
        const auto prefix = "/discord-complete?state=" + state + "&proof=";
        const auto authority = "127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
        if (state.empty() || request.method() != http::verb::get ||
            request[http::field::host] != authority || !target.starts_with(prefix) ||
            !distribution::valid_sha256(target.substr(prefix.size()))) {
          accept();
          return;
        }
        response = {http::status::ok, 11};
        response.keep_alive(false);
        response.set(http::field::content_type, "text/html; charset=utf-8");
        response.set(http::field::cache_control, "no-store");
        response.set("Referrer-Policy", "no-referrer");
        response.set("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
        response.body() =
            "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
            "<title>Project Chronicles sign-in</title><h1>Return to Project Chronicles Launcher</h1>"
            "<p>The sign-in result will appear in the launcher. You can close this tab.</p></html>";
        response.prepare_payload();
        http::async_write(
            stream, response,
            [this, received_proof = target.substr(prefix.size())](beast::error_code, std::size_t) {
              if (stopping) {
                return;
              }
              proof = received_proof;
              beast::error_code close_error;
              stream.socket().shutdown(Tcp::socket::shutdown_both, close_error);
              stream.socket().close(close_error);
            });
      });
    });
  }
};

DiscordLoopback::DiscordLoopback() : impl_(std::make_unique<Impl>()) {}

DiscordLoopback::~DiscordLoopback() = default;

unsigned short DiscordLoopback::port() const {
  return impl_->acceptor.local_endpoint().port();
}

void DiscordLoopback::expect_state(std::string state) {
  if (!distribution::valid_sha256(state)) {
    throw std::invalid_argument("Invalid Discord callback state");
  }
  impl_->state = std::move(state);
}

std::string DiscordLoopback::poll() {
  impl_->context.poll();
  return impl_->proof;
}

} // namespace chronicles::launcher
