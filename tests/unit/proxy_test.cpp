#include "iutest.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sandbox/proxy.hpp"

using namespace cpa::sandbox;

namespace {

class Socket {
 public:
  explicit Socket(int fd = -1) : fd_(fd) {}
  ~Socket() {
    if (fd_ >= 0) ::close(fd_);
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

void timeout(int fd) {
  timeval value{2, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#if defined(SO_NOSIGPIPE)
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

bool send_text(int fd, std::string_view text) {
  while (!text.empty()) {
#if defined(MSG_NOSIGNAL)
    const auto n = ::send(fd, text.data(), text.size(), MSG_NOSIGNAL);
#else
    const auto n = ::send(fd, text.data(), text.size(), 0);
#endif
    if (n <= 0) return false;
    text.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
}

std::string read_all(int fd) {
  std::string result;
  char buffer[4096];
  while (true) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    result.append(buffer, static_cast<std::size_t>(n));
  }
  return result;
}

std::string read_header(int fd) {
  std::string result;
  char c;
  while (!result.ends_with("\r\n\r\n") && result.size() < 65536 && ::recv(fd, &c, 1, 0) == 1) result += c;
  return result;
}

int dial(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("test socket");
  timeout(fd);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<unsigned short>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    throw std::runtime_error("test connect");
  }
  return fd;
}

class LocalServer {
 public:
  explicit LocalServer(std::function<void(int)> handle) : listener_(::socket(AF_INET, SOCK_STREAM, 0)) {
    if (listener_.get() < 0) throw std::runtime_error("test listener");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener_.get(), 16) != 0)
      throw std::runtime_error("test bind");
    socklen_t size = sizeof(addr);
    ::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&addr), &size);
    port = ntohs(addr.sin_port);
    thread_ = std::thread([this, handle = std::move(handle)] {
      while (!done_) {
        pollfd pfd{listener_.get(), POLLIN, 0};
        if (::poll(&pfd, 1, 20) <= 0) continue;
        const Socket client(::accept(listener_.get(), nullptr, nullptr));
        if (client.get() < 0) continue;
        timeout(client.get());
        ++connections;
        handle(client.get());
      }
    });
  }
  ~LocalServer() { stop(); }
  void stop() {
    done_ = true;
    if (thread_.joinable()) thread_.join();
  }
  int port = 0;
  std::atomic<int> connections{0};

 private:
  Socket listener_;
  std::thread thread_;
  std::atomic<bool> done_{false};
};

std::string query_proxy(NetworkProxy& proxy, const std::string& request) {
  Socket client(dial(proxy.port()));
  if (!send_text(client.get(), request)) throw std::runtime_error("test send");
  ::shutdown(client.get(), SHUT_WR);
  return read_all(client.get());
}

std::string authority(int port) { return "127.0.0.1:" + std::to_string(port); }

std::string request(int port, std::string_view method = "GET", std::string_view headers = "",
                    std::string_view body = "", std::string_view path = "/hello?x=1") {
  const auto host = authority(port);
  return std::string(method) + " http://" + host + std::string(path) + " HTTP/1.1\r\nHost: " + host + "\r\n" +
         std::string(headers) + "\r\n" + std::string(body);
}

}  // namespace

IUTEST(ProxyTest, DefaultAndExplicitHostPolicy) {
  for (const auto& host : {"github.com", "API.GITHUB.COM.", "raw.githubusercontent.com", "ghcr.io", "pkg.ghcr.io"})
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
  for (const auto& host : {"github.com.evil.test", "notgithub.com", "example.com", "localhost", "127.0.0.1", "::1"})
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
  IUTEST_EXPECT_TRUE(host_allowed("Example.COM.", {" example.com "}));
  IUTEST_EXPECT_TRUE(host_allowed("a.b.example.com", {"*.example.com"}));
  IUTEST_EXPECT_FALSE(host_allowed("example.com", {"*.example.com"}));
  IUTEST_EXPECT_FALSE(host_allowed("badexample.com", {"*.example.com"}));
  IUTEST_EXPECT_FALSE(host_allowed("example.com", {"*."}));
  IUTEST_EXPECT_TRUE(host_allowed("a.example.com", {"*.EXAMPLE.COM."}));
  IUTEST_EXPECT_TRUE(host_allowed("localhost", {"localhost"}));
  IUTEST_EXPECT_TRUE(host_allowed("::1", {"::1"}));
  for (const auto& host : {"", ".", "a..b", "bad host", "x@y", "foo/bar", "foo\\bar", "foo%00", "-foo", "a.."})
    IUTEST_EXPECT_FALSE(host_allowed(host, {"*"})) << host;
}

IUTEST(ProxyTest, StrictAuthorities) {
  std::string host;
  int port = 0;
  IUTEST_ASSERT_TRUE(split_host_port("example.com", host, port, 80));
  IUTEST_EXPECT_EQ("example.com", host);
  IUTEST_EXPECT_EQ(80, port);
  IUTEST_ASSERT_TRUE(split_host_port("[::1]:443", host, port, 80));
  IUTEST_EXPECT_EQ("::1", host);
  IUTEST_EXPECT_EQ(443, port);
  IUTEST_ASSERT_TRUE(split_host_port("[2001:db8::1]", host, port, 443));
  for (const auto& value : {"",
                            ":80",
                            "host:",
                            "host:0",
                            "host:65536",
                            "host:+80",
                            "host: 80",
                            "host:80junk",
                            "host:-1",
                            "user@host:80",
                            "host/path",
                            "host?query",
                            "host#fragment",
                            "host\\evil",
                            "host%00",
                            "host..",
                            "::1",
                            "[localhost]:80",
                            "[::1]x",
                            "[::1]:",
                            "[::1%lo]:80",
                            "[::1",
                            "[::1]:80:90",
                            "a:1:2",
                            "host:\t80"})
    IUTEST_EXPECT_FALSE(split_host_port(value, host, port, 80)) << value;
}

IUTEST(ProxyTest, DenialCallsCallbackWithoutDialing) {
  LocalServer server([](int) {});
  std::string denied_host;
  std::atomic<int> denied_port{0};
  NetworkProxy proxy(default_allowed_hosts(), [&](const std::string& host, int port) {
    denied_host = host;
    denied_port = port;
  });
  proxy.start();
  IUTEST_EXPECT_TRUE(proxy.url().starts_with("http://127.0.0.1:"));
  IUTEST_EXPECT_TRUE(query_proxy(proxy, request(server.port)).starts_with("HTTP/1.1 403"));
  proxy.stop();
  IUTEST_EXPECT_EQ("127.0.0.1", denied_host);
  IUTEST_EXPECT_EQ(server.port, denied_port.load());
  IUTEST_EXPECT_EQ(0, server.connections.load());
}

IUTEST(ProxyTest, WaitsForRequestAfterAccept) {
  LocalServer server([](int fd) {
    read_header(fd);
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  });
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  Socket client(dial(proxy.port()));
  pollfd fd{client.get(), POLLIN, 0};
  IUTEST_EXPECT_EQ(0, ::poll(&fd, 1, 50));
  IUTEST_ASSERT_TRUE(send_text(client.get(), request(server.port)));
  IUTEST_EXPECT_TRUE(read_all(client.get()).starts_with("HTTP/1.1 200"));
  proxy.stop();
  server.stop();
}

IUTEST(ProxyTest, WildcardCannotAccessPrivateAddressesOrNumericAliases) {
  LocalServer server([](int) {});
  std::atomic<int> denied{0};
  NetworkProxy proxy({"*"}, [&](const std::string&, int) { ++denied; });
  proxy.start();
  const std::vector<std::string> hosts = {
      "127.0.0.1", "localhost",       "127.1",      "2130706433",  "0x7f000001",     "[::ffff:127.0.0.1]", "0.0.0.0",
      "10.0.0.1",  "169.254.169.254", "172.16.0.1", "192.168.0.1", "100.64.0.1",     "198.18.0.1",         "224.0.0.1",
      "[::]",      "[::1]",           "[fc00::1]",  "[fe80::1]",   "[2002:a00:1::]",
  };
  for (const auto& host : hosts) {
    const auto target = std::string(host) + ":" + std::to_string(server.port);
    const auto response = query_proxy(proxy, "CONNECT " + target + " HTTP/1.1\r\n\r\n");
    IUTEST_EXPECT_TRUE(response.starts_with("HTTP/1.1 403")) << host << ": " << response;
  }
  proxy.stop();
  IUTEST_EXPECT_EQ(static_cast<int>(hosts.size()), denied.load());
  IUTEST_EXPECT_EQ(0, server.connections.load());
}

IUTEST(ProxyTest, ConnectTunnelsIncludingBufferedBytesAndHalfClose) {
  LocalServer server([](int fd) { send_text(fd, read_all(fd)); });
  NetworkProxy proxy({"localhost"}, {});
  proxy.start();
  const auto response =
      query_proxy(proxy, "CONNECT localhost:" + std::to_string(server.port) + " HTTP/1.1\r\n\r\nhello tunnel");
  IUTEST_EXPECT_EQ("HTTP/1.1 200 Connection Established\r\n\r\nhello tunnel", response);
}

IUTEST(ProxyTest, HttpForwardsOneFramedRequestAndStripsHopHeaders) {
  std::string received;
  LocalServer server([&](int fd) {
    received = read_all(fd);
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  });
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  const auto response = query_proxy(
      proxy,
      request(server.port, "POST",
              "Content-Length: 4\r\nProxy-Authorization: secret\r\nConnection: keep-alive, X-Hop\r\nX-Hop: hidden\r\n",
              "body"));
  IUTEST_EXPECT_TRUE(response.ends_with("\r\n\r\nok"));
  proxy.stop();
  server.stop();
  IUTEST_EXPECT_TRUE(received.starts_with("POST /hello?x=1 HTTP/1.1\r\nHost: " + authority(server.port) + "\r\n"));
  IUTEST_EXPECT_NE(std::string::npos, received.find("content-length: 4\r\n"));
  IUTEST_EXPECT_TRUE(received.ends_with("Connection: close\r\n\r\nbody"));
  IUTEST_EXPECT_EQ(std::string::npos, received.find("secret"));
  IUTEST_EXPECT_EQ(std::string::npos, received.find("hidden"));
}

IUTEST(ProxyTest, HttpSupportsQueryWithoutPathAndHttp10) {
  std::string received;
  LocalServer server([&](int fd) {
    received = read_all(fd);
    send_text(fd, "HTTP/1.0 200 OK\r\n\r\nok");
  });
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  IUTEST_EXPECT_TRUE(
      query_proxy(proxy, "GET http://" + authority(server.port) + "?query HTTP/1.0\r\n\r\n").ends_with("ok"));
  proxy.stop();
  server.stop();
  IUTEST_EXPECT_TRUE(received.starts_with("GET /?query HTTP/1.0\r\n"));
}

IUTEST(ProxyTest, RejectsMalformedRequestsBeforeDialing) {
  LocalServer server([](int) {});
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  const auto host = authority(server.port);
  const std::string first = "GET http://" + host + "/ HTTP/1.1\r\nHost: " + host + "\r\n";
  const std::vector<std::string> malformed = {
      "",
      "bad\r\n\r\n",
      "GET /relative HTTP/1.1\r\n\r\n",
      "GET https://" + host + "/ HTTP/1.1\r\n\r\n",
      "GET http://user@" + host + "/ HTTP/1.1\r\n\r\n",
      "GET http://" + host + "/ HTTP/2\r\n\r\n",
      "GET http://" + host + "/ HTTP/1.1\r\n\r\n",
      "GET http://" + host + "/#fragment HTTP/1.0\r\n\r\n",
      first + "Host: " + host + "\r\n\r\n",
      first + "Content-Length: -1\r\n\r\n",
      first + "Content-Length: 1\r\nContent-Length: 1\r\n\r\nx",
      first + "Content-Length: 999999999999999999999999999\r\n\r\n",
      first + "Content-Length: 67108865\r\n\r\n",
      first + "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
      first + "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n",
      first + "Upgrade: websocket\r\n\r\n",
      first + "Expect: 100-continue\r\n\r\n",
      first + "Connection: Content-Length\r\n\r\n",
      first + "Connection: Host\r\n\r\n",
      first + " Host: evil\r\n\r\n",
      first + "Invalid Header: value\r\n\r\n",
      first + "X: abc\nInjected: true\r\n\r\n",
      first + "X: abc\r\r\n\r\n",
      first + "Trailer: X\r\n\r\n",
      first + std::string("X: a\0b\r\n\r\n", 10),
      "GET http://" + host + "/ HTTP/1.1\r\nHost: evil.test\r\n\r\n",
      "CONNECT " + host + " HTTP/1.1\r\nContent-Length: 1\r\n\r\nx",
      first + "\r\nGET http://evil.test/ HTTP/1.1\r\nHost: evil.test\r\n\r\n",
      first + "X-Large: " + std::string(65536, 'x') + "\r\n\r\n",
  };
  for (const auto& value : malformed) {
    const auto response = query_proxy(proxy, value);
    IUTEST_EXPECT_TRUE(response.starts_with("HTTP/1.1 400")) << value.substr(0, 150) << ": " << response;
  }
  proxy.stop();
  IUTEST_EXPECT_EQ(0, server.connections.load());
}

IUTEST(ProxyTest, DelayedSecondRequestIsNeverRelayed) {
  std::string received;
  std::atomic<bool> got_first{false};
  std::atomic<bool> sent_second{false};
  LocalServer server([&](int fd) {
    received = read_header(fd);
    got_first = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!sent_second && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    received += read_all(fd);
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
  });
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  Socket client(dial(proxy.port()));
  IUTEST_ASSERT_TRUE(send_text(client.get(), request(server.port)));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!got_first && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool first = got_first;
  send_text(client.get(), "GET http://evil.test/ HTTP/1.1\r\nHost: evil.test\r\n\r\n");
  sent_second = true;
  IUTEST_EXPECT_TRUE(first);
  IUTEST_EXPECT_TRUE(read_all(client.get()).ends_with("ok"));
  proxy.stop();
  server.stop();
  IUTEST_EXPECT_EQ(std::string::npos, received.find("evil.test"));
}

IUTEST(ProxyTest, StreamsOnlyContentLengthBytes) {
  std::string received;
  std::atomic<bool> got_header{false};
  LocalServer server([&](int fd) {
    received = read_header(fd);
    got_header = true;
    received += read_all(fd);
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
  });
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  Socket client(dial(proxy.port()));
  IUTEST_ASSERT_TRUE(send_text(client.get(), request(server.port, "POST", "Content-Length: 4\r\n")));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!got_header && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  IUTEST_EXPECT_TRUE(send_text(client.get(), "bodyGET http://evil.test/ HTTP/1.1\r\n\r\n"));
  IUTEST_EXPECT_TRUE(read_all(client.get()).starts_with("HTTP/1.1 200"));
  proxy.stop();
  server.stop();
  IUTEST_EXPECT_TRUE(received.ends_with("\r\n\r\nbody"));
  IUTEST_EXPECT_EQ(std::string::npos, received.find("evil.test"));
}

IUTEST(ProxyTest, FailedLocalConnectionReturnsBadGateway) {
  int unused_port = 0;
  {
    LocalServer server([](int) {});
    unused_port = server.port;
  }
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  IUTEST_EXPECT_TRUE(query_proxy(proxy, request(unused_port)).starts_with("HTTP/1.1 502"));
}

IUTEST(ProxyTest, StopInterruptsIncompleteRequestBody) {
  LocalServer server([](int fd) { read_all(fd); });
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  Socket client(dial(proxy.port()));
  IUTEST_ASSERT_TRUE(send_text(client.get(), request(server.port, "POST", "Content-Length: 100\r\n", "a")));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (server.connections == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  IUTEST_EXPECT_EQ(1, server.connections.load());
  const auto start = std::chrono::steady_clock::now();
  proxy.stop();
  IUTEST_EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

IUTEST(ProxyTest, StopClosesIdleClientsAndTunnelsAndCanRestart) {
  std::atomic<bool> tunnel_closed{false};
  LocalServer server([&](int fd) {
    read_all(fd);
    tunnel_closed = true;
  });
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  proxy.start();
  Socket idle(dial(proxy.port()));
  Socket tunnel(dial(proxy.port()));
  IUTEST_ASSERT_TRUE(send_text(tunnel.get(), "CONNECT " + authority(server.port) + " HTTP/1.1\r\n\r\n"));
  IUTEST_EXPECT_TRUE(read_header(tunnel.get()).starts_with("HTTP/1.1 200"));
  const auto start = std::chrono::steady_clock::now();
  proxy.stop();
  proxy.stop();
  server.stop();
  IUTEST_EXPECT_TRUE(tunnel_closed.load());
  IUTEST_EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
  IUTEST_EXPECT_TRUE(read_all(tunnel.get()).empty());
  proxy.start();
  IUTEST_EXPECT_TRUE(query_proxy(proxy, "invalid\r\n\r\n").starts_with("HTTP/1.1 400"));
  proxy.stop();
}

IUTEST(ProxyTest, IdleClientFloodDoesNotPreventShutdown) {
  NetworkProxy proxy({}, {});
  proxy.start();
  std::vector<std::unique_ptr<Socket>> clients;
  for (int i = 0; i < 48; ++i) clients.push_back(std::make_unique<Socket>(dial(proxy.port())));
  std::vector<pollfd> fds;
  for (const auto& client : clients) fds.push_back({client->get(), POLLIN, 0});
  bool rejected = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!rejected && std::chrono::steady_clock::now() < deadline) {
    for (auto& fd : fds) fd.revents = 0;
    if (::poll(fds.data(), fds.size(), 50) <= 0) continue;
    for (const auto& fd : fds) {
      if (!fd.revents) continue;
      char c;
      const auto n = ::recv(fd.fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) rejected = true;
    }
  }
  IUTEST_EXPECT_TRUE(rejected);
  const auto start = std::chrono::steady_clock::now();
  proxy.stop();
  IUTEST_EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

IUTEST(ProxyTest, ThrowingDenialCallbackDoesNotTerminateWorker) {
  NetworkProxy proxy({}, [](const std::string&, int) { throw std::runtime_error("callback"); });
  proxy.start();
  query_proxy(proxy, "CONNECT localhost:80 HTTP/1.1\r\n\r\n");
  proxy.stop();
}
