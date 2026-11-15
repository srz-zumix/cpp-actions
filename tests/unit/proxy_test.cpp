#include "iutest.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sandbox/connect.hpp"
#include "sandbox/proxy.hpp"

using namespace cpa::sandbox;

namespace cpa::sandbox {
struct NetworkProxyTestAccess {
  static bool allowed(const NetworkProxy& proxy, std::string_view host) {
    return host_allowed(host, proxy.allowed_);
  }
  static bool private_allowed(const NetworkProxy& proxy, std::string_view host) {
    return host_allowed(host, proxy.private_allowed_);
  }
};
}  // namespace cpa::sandbox

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

// Reads one request framed by Content-Length, as a real HTTP server would,
// without waiting for the peer to close its sending side.
std::string read_request(int fd) {
  auto result = read_header(fd);
  std::size_t length = 0;
  const auto lower = [](std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
  }(result);
  if (const auto pos = lower.find("\r\ncontent-length:"); pos != std::string::npos)
    length = std::stoul(result.substr(pos + 17));
  std::string body(length, '\0');
  std::size_t offset = 0;
  while (offset < length) {
    const auto n = ::recv(fd, body.data() + offset, length - offset, 0);
    if (n <= 0) break;
    offset += static_cast<std::size_t>(n);
  }
  return result + body.substr(0, offset);
}

// Returns bytes that arrive within `wait` without blocking until EOF.
std::string read_available(int fd, std::chrono::milliseconds wait) {
  std::string result;
  char buffer[4096];
  pollfd pfd{fd, POLLIN, 0};
  while (::poll(&pfd, 1, static_cast<int>(wait.count())) > 0) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    result.append(buffer, static_cast<std::size_t>(n));
  }
  return result;
}

int dial(int port, bool ipv6 = false) {
  const int fd = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("test socket");
  timeout(fd);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<unsigned short>(port));
  sockaddr_in6 addr6{};
  addr6.sin6_family = AF_INET6;
  addr6.sin6_addr = in6addr_loopback;
  addr6.sin6_port = addr.sin_port;
  const int connected = ipv6 ? ::connect(fd, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6))
                             : ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (connected != 0) {
    ::close(fd);
    throw std::runtime_error("test connect");
  }
  return fd;
}

class LocalServer {
 public:
  explicit LocalServer(std::function<void(int)> handle, bool ipv6 = false)
      : listener_(::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0)) {
    if (listener_.get() < 0) throw std::runtime_error("test listener");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_addr = in6addr_loopback;
    const auto* bind_address = ipv6 ? reinterpret_cast<const sockaddr*>(&addr6)
                                    : reinterpret_cast<const sockaddr*>(&addr);
    if (::bind(listener_.get(), bind_address, ipv6 ? sizeof(addr6) : sizeof(addr)) != 0 ||
        ::listen(listener_.get(), 16) != 0)
      throw std::runtime_error("test bind");
    sockaddr_storage bound{};
    socklen_t size = sizeof(bound);
    ::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&bound), &size);
    port = ntohs(ipv6 ? reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port
                      : reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
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

struct ScriptedConnector {
  using Milliseconds = std::chrono::milliseconds;
  struct Plan {
    Milliseconds delay = Milliseconds::max();
    int error = 0;
    bool immediate = false;
    int finish_error = 0;
    bool socket_failure = false;
  };
  struct Launch {
    std::size_t address;
    Milliseconds time;
  };
  std::vector<Plan> plans;
  std::vector<Launch> launches;
  std::vector<std::pair<std::size_t, int>> errors;
  std::set<int> open;
  std::vector<int> closed;
  Milliseconds elapsed{0};
  Milliseconds stop_at = Milliseconds::max();
  bool stop_on_finish = false;
  bool throw_on_record = false;
  bool stop = false;

  auto now() const { return std::chrono::steady_clock::time_point{} + elapsed; }
  bool stopped() const { return stop || elapsed >= stop_at; }
  detail::ConnectionAttempt start(std::size_t index) {
    launches.push_back({index, elapsed});
    if (plans[index].socket_failure) return {-1, EMFILE};
    const int fd = static_cast<int>(index) + 100;
    open.insert(fd);
    return {fd, plans[index].immediate ? plans[index].error : EINPROGRESS};
  }
  void wait(std::vector<detail::PendingConnection>& pending, Milliseconds timeout) {
    auto until = elapsed + timeout;
    until = std::min(until, stop_at);
    for (const auto& socket : pending) {
      const auto& plan = plans[socket.address];
      if (plan.delay != Milliseconds::max())
        until = std::min(until, launches[socket.address].time + plan.delay);
    }
    elapsed = until;
    for (auto& socket : pending) {
      const auto& plan = plans[socket.address];
      if (plan.delay != Milliseconds::max() && elapsed >= launches[socket.address].time + plan.delay)
        socket.error = plan.error;
    }
  }
  int finish(int fd) {
    if (stop_on_finish) stop = true;
    return plans[static_cast<std::size_t>(fd - 100)].finish_error;
  }
  void close(int fd) {
    if (open.erase(fd) != 1) throw std::runtime_error("socket closed twice");
    closed.push_back(fd);
  }
  void record(std::size_t index, int error) {
    if (throw_on_record) throw std::runtime_error("test diagnostic failure");
    errors.emplace_back(index, error);
  }
};

}  // namespace

IUTEST(ProxyTest, AddressFamiliesAreInterleavedWithoutChangingWithinFamilyOrder) {
  for (int first_family : {AF_INET6, AF_INET}) {
    addrinfo addresses[5]{};
    for (std::size_t i = 0; i < 5; ++i) {
      addresses[i].ai_family = i < 3 ? first_family : (first_family == AF_INET ? AF_INET6 : AF_INET);
      addresses[i].ai_next = i + 1 < 5 ? &addresses[i + 1] : nullptr;
    }
    const auto ordered = detail::interleave_addresses(addresses);
    IUTEST_ASSERT_EQ(5u, ordered.size());
    const std::size_t expected[] = {0, 3, 1, 4, 2};
    for (std::size_t i = 0; i < 5; ++i) IUTEST_EXPECT_EQ(&addresses[expected[i]], ordered[i]);
  }
  IUTEST_EXPECT_TRUE(detail::interleave_addresses(nullptr).empty());
}

IUTEST(ProxyTest, StalledFirstAddressDoesNotDelayAlternateFamily) {
  using namespace std::chrono_literals;
  ScriptedConnector connector;
  connector.plans = {{}, {10ms}};
  const int winner = detail::race_connections(connector.plans.size(), connector);
  IUTEST_EXPECT_EQ(101, winner);
  IUTEST_EXPECT_EQ(260ms, connector.elapsed);
  IUTEST_ASSERT_EQ(2u, connector.launches.size());
  IUTEST_EXPECT_EQ(0u, connector.launches[0].address);
  IUTEST_EXPECT_EQ(0ms, connector.launches[0].time);
  IUTEST_EXPECT_EQ(1u, connector.launches[1].address);
  IUTEST_EXPECT_EQ(250ms, connector.launches[1].time);
  IUTEST_EXPECT_EQ(0u, connector.open.count(100));
  IUTEST_EXPECT_EQ(1u, connector.open.count(winner));
  connector.close(winner);
}

IUTEST(ProxyTest, DefinitiveErrorsAccelerateFallbackWhileAnotherAttemptIsPending) {
  using namespace std::chrono_literals;
  for (bool immediate : {false, true}) {
    ScriptedConnector connector;
    connector.plans = {{}, {10ms, ECONNREFUSED, immediate}, {1ms}};
    IUTEST_EXPECT_EQ(102, detail::race_connections(connector.plans.size(), connector));
    IUTEST_ASSERT_EQ(3u, connector.launches.size());
    IUTEST_EXPECT_EQ(immediate ? 250ms : 260ms, connector.launches[2].time);
    IUTEST_ASSERT_EQ(1u, connector.errors.size());
    IUTEST_EXPECT_EQ(ECONNREFUSED, connector.errors.front().second);
    IUTEST_EXPECT_EQ(1u, connector.open.size());
    connector.close(102);
  }
}

IUTEST(ProxyTest, TcpBudgetAllowsSuccessAfterOldThreeSecondDeadline) {
  using namespace std::chrono_literals;
  IUTEST_EXPECT_EQ(15s, detail::kDnsTimeout);
  IUTEST_EXPECT_EQ(15s, detail::kTcpTimeout);
  ScriptedConnector connector;
  connector.plans = {{6s}, {}};
  const int winner = detail::race_connections(connector.plans.size(), connector);
  IUTEST_EXPECT_EQ(100, winner);
  IUTEST_EXPECT_EQ(6s, connector.elapsed);
  IUTEST_EXPECT_EQ(1u, connector.open.size());
  IUTEST_EXPECT_EQ(0u, connector.open.count(101));
  connector.close(winner);
}

IUTEST(ProxyTest, ExhaustedAddressesAndSocketAllocationErrorsFailWithoutWaitingOrLeaks) {
  using namespace std::chrono_literals;
  ScriptedConnector connector;
  connector.plans = {{0ms, ECONNREFUSED, true}, {0ms, 0, false, 0, true}, {10ms, ENETUNREACH}};
  IUTEST_EXPECT_EQ(-1, detail::race_connections(connector.plans.size(), connector));
  IUTEST_EXPECT_EQ(10ms, connector.elapsed);
  IUTEST_EXPECT_TRUE(connector.open.empty());
  IUTEST_ASSERT_EQ(3u, connector.errors.size());
  IUTEST_EXPECT_EQ(ECONNREFUSED, connector.errors[0].second);
  IUTEST_EXPECT_EQ(EMFILE, connector.errors[1].second);
  IUTEST_EXPECT_EQ(ENETUNREACH, connector.errors[2].second);
  IUTEST_EXPECT_EQ(-1, detail::race_connections(0, connector));
}

IUTEST(ProxyTest, AllStalledAttemptsShareOneBoundedTcpDeadlineAndAreClosed) {
  using namespace std::chrono_literals;
  ScriptedConnector connector;
  connector.plans.resize(70);
  IUTEST_EXPECT_EQ(-1, detail::race_connections(connector.plans.size(), connector));
  IUTEST_EXPECT_EQ(15s, connector.elapsed);
  IUTEST_EXPECT_EQ(60u, connector.launches.size());
  IUTEST_EXPECT_EQ(60u, connector.closed.size());
  IUTEST_EXPECT_TRUE(connector.open.empty());
  IUTEST_ASSERT_EQ(60u, connector.errors.size());
  for (const auto& error : connector.errors) IUTEST_EXPECT_EQ(ETIMEDOUT, error.second);
}

IUTEST(ProxyTest, StoppingInterruptsConnectionRaceAndClosesEveryAttempt) {
  using namespace std::chrono_literals;
  ScriptedConnector connector;
  connector.plans.resize(5);
  connector.stop_at = 725ms;
  IUTEST_EXPECT_EQ(-1, detail::race_connections(connector.plans.size(), connector));
  IUTEST_EXPECT_LT(connector.elapsed, 800ms);
  IUTEST_EXPECT_EQ(3u, connector.launches.size());
  IUTEST_EXPECT_TRUE(connector.open.empty());
  IUTEST_EXPECT_TRUE(connector.errors.empty());
}

IUTEST(ProxyTest, SimultaneousSuccessesKeepOnlyOneSocketAndRestoreFailuresTryNext) {
  using namespace std::chrono_literals;
  for (int finish_error : {0, EIO}) {
    ScriptedConnector connector;
    connector.plans = {{300ms, 0, false, finish_error}, {50ms}};
    const int winner = detail::race_connections(connector.plans.size(), connector);
    IUTEST_EXPECT_EQ(finish_error == 0 ? 100 : 101, winner);
    IUTEST_EXPECT_EQ(300ms, connector.elapsed);
    IUTEST_EXPECT_EQ(1u, connector.open.size());
    connector.close(winner);
  }
}

IUTEST(ProxyTest, StopDuringWinnerSelectionAndExceptionsClosePendingSockets) {
  using namespace std::chrono_literals;
  ScriptedConnector stopping;
  stopping.plans = {{300ms}, {}};
  stopping.stop_on_finish = true;
  IUTEST_EXPECT_EQ(-1, detail::race_connections(stopping.plans.size(), stopping));
  IUTEST_EXPECT_TRUE(stopping.open.empty());
  ScriptedConnector throwing;
  throwing.plans = {{}, {1ms, ECONNREFUSED}};
  throwing.throw_on_record = true;
  IUTEST_EXPECT_THROW(detail::race_connections(throwing.plans.size(), throwing), std::runtime_error);
  IUTEST_EXPECT_TRUE(throwing.open.empty());
}

IUTEST(ProxyTest, Ipv6UpstreamConnectionRelaysThroughConnectTunnel) {
  LocalServer server([](int fd) {
    read_header(fd);
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nipv6");
  }, true);
  NetworkProxy proxy({"::1"}, {});
  proxy.start();
  Socket client(dial(proxy.port()));
  IUTEST_ASSERT_TRUE(send_text(client.get(), "CONNECT [::1]:" + std::to_string(server.port) +
                                           " HTTP/1.1\r\n\r\n"));
  IUTEST_EXPECT_TRUE(read_header(client.get()).starts_with("HTTP/1.1 200 Connection Established"));
  IUTEST_ASSERT_TRUE(send_text(client.get(), "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));
  IUTEST_EXPECT_TRUE(read_all(client.get()).ends_with("ipv6"));
  proxy.stop();
}

IUTEST(ProxyTest, DualStackLocalhostCanReachAnIpv4OnlyUpstream) {
  LocalServer server([](int fd) {
    read_header(fd);
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nipv4");
  });
  NetworkProxy proxy({"localhost"}, {});
  proxy.start();
  const auto host = "localhost:" + std::to_string(server.port);
  const auto response = query_proxy(proxy, "GET http://" + host + "/ HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
  IUTEST_EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
  IUTEST_EXPECT_TRUE(response.ends_with("ipv4"));
  proxy.stop();
}

IUTEST(ProxyTest, UpstreamFailureWarningsExcludeRequestSecretsAndPolicyDenials) {
  int port = 0;
  {
    LocalServer server([](int) {});
    port = server.port;
  }
  std::mutex mutex;
  std::vector<std::string> warnings;
  std::atomic<int> denials{0};
  NetworkProxy proxy({"127.0.0.1"}, [&](const auto&, int) { ++denials; }, [&](std::string_view warning) {
    std::lock_guard lock(mutex);
    warnings.emplace_back(warning);
  });
  proxy.start();
  for (const auto& message : {
           "CONNECT " + authority(port) + " HTTP/1.1\r\nProxy-Authorization: Bearer secret-token\r\n\r\n",
           request(port, "GET", "Authorization: Bearer secret-token\r\n", "", "/private?token=secret-token")}) {
    IUTEST_EXPECT_TRUE(query_proxy(proxy, message).starts_with("HTTP/1.1 502"));
  }
  IUTEST_EXPECT_TRUE(query_proxy(proxy, "CONNECT forbidden.invalid:443 HTTP/1.1\r\n\r\n")
                         .starts_with("HTTP/1.1 403"));
  LocalServer server([](int fd) {
    read_header(fd);
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
  });
  IUTEST_EXPECT_TRUE(query_proxy(proxy, request(server.port)).starts_with("HTTP/1.1 200"));
  proxy.stop();
  IUTEST_EXPECT_EQ(1, denials.load());
  IUTEST_ASSERT_EQ(2u, warnings.size());
  for (const auto& warning : warnings) {
    IUTEST_EXPECT_NE(std::string::npos, warning.find(authority(port)));
    IUTEST_EXPECT_NE(std::string::npos, warning.find("TCP connection failed"));
    IUTEST_EXPECT_NE(std::string::npos, warning.find("errno=" + std::to_string(ECONNREFUSED)));
    IUTEST_EXPECT_NE(std::string::npos, warning.find(" ms: "));
    IUTEST_EXPECT_NE(std::string::npos, warning.find("HTTP 502"));
    IUTEST_EXPECT_NE(std::string::npos, warning.find("(at "));
    IUTEST_EXPECT_NE(std::string::npos, warning.find("active "));
    IUTEST_EXPECT_NE(std::string::npos, warning.find("/128, connecting upstream "));
    IUTEST_EXPECT_EQ(std::string::npos, warning.find("secret-token"));
    IUTEST_EXPECT_EQ(std::string::npos, warning.find("/private"));
    IUTEST_EXPECT_EQ(std::string::npos, warning.find("Authorization"));
  }
}

IUTEST(ProxyTest, DnsFailureWarningIdentifiesResolutionStage) {
  std::string warning;
  NetworkProxy proxy({"proxy-dns-failure.invalid"}, {}, [&](std::string_view message) { warning = message; });
  proxy.start();
  Socket client(dial(proxy.port()));
  timeval value{20, 0};
  ::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  IUTEST_ASSERT_TRUE(send_text(client.get(), "CONNECT proxy-dns-failure.invalid:443 HTTP/1.1\r\n\r\n"));
  IUTEST_EXPECT_TRUE(read_header(client.get()).starts_with("HTTP/1.1 502"));
  proxy.stop();
  IUTEST_EXPECT_NE(std::string::npos, warning.find("proxy-dns-failure.invalid:443"));
  IUTEST_EXPECT_NE(std::string::npos, warning.find("DNS resolution"));
  IUTEST_EXPECT_TRUE(warning.find("getaddrinfo=") != std::string::npos ||
                     warning.find("DNS deadline: 15000 ms") != std::string::npos);
}

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

IUTEST(ProxyTest, UnityHostPresetIsExactOptInAndDoesNotAuthorizePrivateAddresses) {
  const std::vector<std::string> expected = {
      "packages.unity.com", "cdn.packages.unity.com", "download.packages.unity.com",
      "public-cdn.cloud.unity3d.com", "license.unity3d.com", "cdp.cloud.unity3d.com",
      "config.uca.cloud.unity3d.com",
      "crl3.digicert.com", "crl4.digicert.com",
  };
  IUTEST_EXPECT_EQ(expected, unity_allowed_hosts());
  for (const bool enabled : {false, true}) {
    NetworkProxy proxy(default_allowed_hosts(), {}, {}, enabled);
    for (const auto& host : expected) {
      IUTEST_EXPECT_EQ(enabled, NetworkProxyTestAccess::allowed(proxy, host)) << host;
      IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(proxy, host)) << host;
      for (const auto& near : {"sub." + host, host + ".evil.test", "not" + host})
        IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::allowed(proxy, near)) << near;
    }
    for (const auto* host : {"registry.example.test", "service.example.test",
                              "10.0.0.1", "127.0.0.1", "unity.com", "cloud.unity3d.com",
                              "unknown.unity3d.com", "crl.digicert.com"})
      IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
    NetworkProxy explicit_hosts({"10.0.0.1", "license.unity3d.com", "github.com", "*.example.test",
                                  "registry.example.test", "service.example.test"},
                                 {}, {}, enabled);
    for (const auto* host : {"registry.example.test", "service.example.test"}) {
      IUTEST_EXPECT_TRUE(NetworkProxyTestAccess::allowed(explicit_hosts, host));
      IUTEST_EXPECT_TRUE(NetworkProxyTestAccess::private_allowed(explicit_hosts, host));
    }
    IUTEST_EXPECT_TRUE(NetworkProxyTestAccess::private_allowed(explicit_hosts, "10.0.0.1"));
    IUTEST_EXPECT_TRUE(NetworkProxyTestAccess::private_allowed(explicit_hosts, "license.unity3d.com"));
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(explicit_hosts, "github.com"));
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(explicit_hosts, "sub.example.test"));
  }
}

IUTEST(ProxyTest, MixedPublicPrivateDnsAnswersAreRejectedBeforeAddressRacing) {
  // Synthetic global-unicast addresses are only classified in memory, never resolved or dialed.
  sockaddr_in public_v4{};
  public_v4.sin_family = AF_INET;
  public_v4.sin_addr.s_addr = htonl(0x12345678);
  sockaddr_in6 public_v6{};
  public_v6.sin6_family = AF_INET6;
  public_v6.sin6_addr.s6_addr[0] = 0x20;
  public_v6.sin6_addr.s6_addr[1] = 0x03;
  public_v6.sin6_addr.s6_addr[15] = 1;
  addrinfo first{};
  first.ai_addr = reinterpret_cast<sockaddr*>(&public_v4);
  addrinfo second{};
  second.ai_addr = reinterpret_cast<sockaddr*>(&public_v6);
  first.ai_next = &second;
  IUTEST_EXPECT_TRUE(detail::public_addresses(&first));
  sockaddr_in6 private_v6{};
  private_v6.sin6_family = AF_INET6;
  for (const auto* host : {"::1", "fd00::1", "::ffff:10.0.0.1", "::ffff:127.0.0.1"}) {
    IUTEST_ASSERT_EQ(1, ::inet_pton(AF_INET6, host, &private_v6.sin6_addr));
    second.ai_addr = reinterpret_cast<sockaddr*>(&private_v6);
    IUTEST_EXPECT_FALSE(detail::public_addresses(&first)) << host;
    IUTEST_EXPECT_FALSE(detail::public_addresses(&second)) << host;
  }
  sockaddr_in private_v4{};
  private_v4.sin_family = AF_INET;
  IUTEST_ASSERT_EQ(1, ::inet_pton(AF_INET, "10.0.0.1", &private_v4.sin_addr));
  first.ai_addr = reinterpret_cast<sockaddr*>(&private_v4);
  second.ai_addr = reinterpret_cast<sockaddr*>(&public_v6);
  IUTEST_EXPECT_FALSE(detail::public_addresses(&first));
}

IUTEST(ProxyTest, BothLoopbackFamiliesForwardAndRestartOnTheSamePort) {
  LocalServer server([](int fd) {
    read_header(fd);
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  });
  NetworkProxy proxy({"127.0.0.1"}, {});
  for (int iteration = 0; iteration < 2; ++iteration) {
    proxy.start();
    IUTEST_EXPECT_EQ("http://localhost:" + std::to_string(proxy.port()), proxy.url());
    for (const bool ipv6 : {false, true}) {
      Socket client(dial(proxy.port(), ipv6));
      IUTEST_ASSERT_TRUE(send_text(client.get(), request(server.port)));
      IUTEST_EXPECT_TRUE(read_all(client.get()).starts_with("HTTP/1.1 200")) << ipv6;
    }
    proxy.stop();
    for (const bool ipv6 : {false, true})
      IUTEST_EXPECT_THROW(dial(proxy.port(), ipv6), std::runtime_error);
  }
  IUTEST_EXPECT_EQ(4, server.connections.load());
}

IUTEST(ProxyTest, DotnetDownloadHostsAreAllowedWithoutWildcards) {
  for (const auto& host : {"builds.dotnet.microsoft.com", "BUILDS.DOTNET.MICROSOFT.COM.",
                           "ci.dot.net", "CI.DOT.NET."})
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
  for (const auto& host : {"microsoft.com", "dotnet.microsoft.com", "dot.net",
                           "sub.builds.dotnet.microsoft.com", "sub.ci.dot.net",
                           "builds.dotnet.microsoft.com.evil.test", "ci.dot.net.evil.test",
                           "notbuilds.dotnet.microsoft.com", "notci.dot.net"})
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
}

IUTEST(ProxyTest, MicrosoftRedirectHostIsAllowedWithoutWildcards) {
  for (const auto& host : {"aka.ms", "AKA.MS."})
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
  for (const auto& host : {"sub.aka.ms", "aka.ms.evil.test", "notaka.ms"})
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
}

IUTEST(ProxyTest, NugetApiHostIsAllowedWithoutWildcards) {
  for (const auto& host : {"api.nuget.org", "API.NUGET.ORG."})
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
  for (const auto& host : {"nuget.org", "sub.api.nuget.org", "globalcdn.nuget.org",
                           "api.nuget.org.evil.test", "notapi.nuget.org"})
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
}

IUTEST(ProxyTest, NugetStorageHostIsAllowedWithoutWildcards) {
  for (const auto& host : {"nugetregistryv2prod.blob.core.windows.net",
                           "NUGETREGISTRYV2PROD.BLOB.CORE.WINDOWS.NET."})
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
  for (const auto& host : {"blob.core.windows.net", "other.blob.core.windows.net",
                           "sub.nugetregistryv2prod.blob.core.windows.net",
                           "nugetregistryv2prod.blob.core.windows.net.evil.test",
                           "notnugetregistryv2prod.blob.core.windows.net"})
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
}

IUTEST(ProxyTest, RubyGemsHostsAreAllowedWithoutWildcardsOrPrivateAddressExceptions) {
  NetworkProxy proxy(default_allowed_hosts(), {});
  for (const auto& host : {"rubygems.org", "RUBYGEMS.ORG.", "index.rubygems.org", "INDEX.RUBYGEMS.ORG."}) {
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
    IUTEST_EXPECT_TRUE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(proxy, host)) << host;
  }
  for (const auto& host : {"sub.rubygems.org", "sub.index.rubygems.org", "rubygems.org.evil.test",
                           "index.rubygems.org.evil.test", "notrubygems.org", "notindex.rubygems.org"})
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
  NetworkProxy explicit_hosts({"rubygems.org", "index.rubygems.org"}, {});
  for (const auto& host : {"rubygems.org", "index.rubygems.org"})
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(explicit_hosts, host)) << host;
}

IUTEST(ProxyTest, GoogleDownloadHostsAreAllowedWithoutWildcardsOrPrivateAddressExceptions) {
  NetworkProxy proxy(default_allowed_hosts(), {});
  for (const auto& host : {"dl.google.com", "DL.GOOGLE.COM.", "maven.google.com", "MAVEN.GOOGLE.COM."}) {
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
    IUTEST_EXPECT_TRUE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(proxy, host)) << host;
  }
  for (const auto& host : {"google.com", "other.google.com", "sub.dl.google.com",
                           "sub.maven.google.com", "dl.google.com.evil.test", "maven.google.com.evil.test",
                           "notdl.google.com", "notmaven.google.com"}) {
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
  }
  NetworkProxy explicit_hosts({"dl.google.com", "maven.google.com"}, {});
  for (const auto& host : {"dl.google.com", "maven.google.com"})
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(explicit_hosts, host)) << host;
}

IUTEST(ProxyTest, MavenAndGradleHostsAreAllowedWithoutWildcardsOrPrivateAddressExceptions) {
  NetworkProxy proxy(default_allowed_hosts(), {});
  for (const auto& host : {"repo.maven.apache.org", "REPO.MAVEN.APACHE.ORG.",
                           "plugins.gradle.org", "PLUGINS.GRADLE.ORG."}) {
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
    IUTEST_EXPECT_TRUE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(proxy, host)) << host;
  }
  for (const auto& host : {"apache.org", "maven.apache.org", "gradle.org",
                           "sub.repo.maven.apache.org", "sub.plugins.gradle.org",
                           "repo.maven.apache.org.evil.test", "plugins.gradle.org.evil.test",
                           "notrepo.maven.apache.org", "notplugins.gradle.org"}) {
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
  }
  NetworkProxy explicit_hosts({"repo.maven.apache.org", "plugins.gradle.org"}, {});
  for (const auto& host : {"repo.maven.apache.org", "plugins.gradle.org"})
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(explicit_hosts, host)) << host;
}

IUTEST(ProxyTest, AdoptiumApiHostIsAllowedWithoutWildcardsOrPrivateAddressExceptions) {
  NetworkProxy proxy(default_allowed_hosts(), {});
  for (const auto& host : {"api.adoptium.net", "API.ADOPTIUM.NET."}) {
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
    IUTEST_EXPECT_TRUE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(proxy, host)) << host;
  }
  for (const auto& host : {"adoptium.net", "other.adoptium.net", "sub.api.adoptium.net",
                           "api.adoptium.net.evil.test", "notapi.adoptium.net"}) {
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
  }
  NetworkProxy explicit_hosts({"api.adoptium.net"}, {});
  IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(explicit_hosts, "api.adoptium.net"));
}

IUTEST(ProxyTest, AppleCertificateHostIsAllowedWithoutWildcardsOrPrivateAddressExceptions) {
  NetworkProxy proxy(default_allowed_hosts(), {});
  for (const auto& host : {"www.apple.com", "WWW.APPLE.COM."}) {
    IUTEST_EXPECT_TRUE(host_allowed(host, default_allowed_hosts())) << host;
    IUTEST_EXPECT_TRUE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(proxy, host)) << host;
  }
  for (const auto& host : {"apple.com", "developer.apple.com", "sub.www.apple.com",
                           "www.apple.com.evil.test", "notwww.apple.com"}) {
    IUTEST_EXPECT_FALSE(host_allowed(host, default_allowed_hosts())) << host;
    IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::allowed(proxy, host)) << host;
  }
  NetworkProxy explicit_hosts({"www.apple.com"}, {});
  IUTEST_EXPECT_FALSE(NetworkProxyTestAccess::private_allowed(explicit_hosts, "www.apple.com"));
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
  IUTEST_EXPECT_TRUE(proxy.url().starts_with("http://localhost:"));
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
      "10.0.0.1",  "169.254.0.1",     "172.16.0.1", "192.168.0.1", "100.64.0.1",     "198.18.0.1",         "224.0.0.1",
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
    received = read_request(fd);
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
    received = read_request(fd);
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

IUTEST(ProxyTest, HttpRequestsDoNotHalfCloseTheUpstream) {
  // Some servers drop a request without responding once they see EOF.
  std::atomic<bool> saw_eof{false};
  LocalServer server([&](int fd) {
    read_request(fd);
    pollfd pfd{fd, POLLIN, 0};
    char byte;
    if (::poll(&pfd, 1, 200) > 0 && ::recv(fd, &byte, 1, MSG_PEEK) == 0) {
      saw_eof = true;
      return;
    }
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
  });
  NetworkProxy proxy({"127.0.0.1"}, {});
  proxy.start();
  for (const auto& message : {request(server.port), request(server.port, "POST", "Content-Length: 2\r\n", "{}")}) {
    Socket client(dial(proxy.port()));
    IUTEST_ASSERT_TRUE(send_text(client.get(), message));
    const auto response = read_all(client.get());
    IUTEST_EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
    IUTEST_EXPECT_TRUE(response.ends_with("ok")) << response;
  }
  proxy.stop();
  server.stop();
  IUTEST_EXPECT_FALSE(saw_eof.load());
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
    received += read_available(fd, std::chrono::milliseconds(200));
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
    received += read_available(fd, std::chrono::milliseconds(300));
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
  Socket idle_v6(dial(proxy.port(), true));
  Socket tunnel(dial(proxy.port(), true));
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
  std::atomic<bool> waiting{false};
  NetworkProxy proxy({}, {}, [&](std::string_view) { waiting = true; }, false, 32);
  proxy.start();
  std::vector<std::unique_ptr<Socket>> clients;
  for (int i = 0; i < 48; ++i) clients.push_back(std::make_unique<Socket>(dial(proxy.port())));
  std::vector<pollfd> fds;
  for (const auto& client : clients) fds.push_back({client->get(), POLLIN, 0});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!waiting && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  IUTEST_EXPECT_TRUE(waiting.load());
  IUTEST_EXPECT_EQ(0, ::poll(fds.data(), fds.size(), 100));
  const auto start = std::chrono::steady_clock::now();
  proxy.stop();
  IUTEST_EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
  proxy.start();
  IUTEST_EXPECT_TRUE(query_proxy(proxy, "CONNECT localhost:80 HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 403"));
  proxy.stop();
}

IUTEST(ProxyTest, SaturatedTunnelsQueueBothFamiliesAndResumeWhenSlotsAreReleased) {
  std::mutex mutex;
  std::vector<std::unique_ptr<Socket>> upstreams;
  LocalServer server([&](int fd) {
    std::lock_guard lock(mutex);
    upstreams.push_back(std::make_unique<Socket>(::dup(fd)));
    send_text(fd, "pong");
  });
  std::atomic<bool> waiting{false};
  std::string capacity_warning;
  NetworkProxy proxy({"127.0.0.1"}, {}, [&](std::string_view message) {
    if (message.find("32 active connections") == std::string_view::npos) return;
    {
      std::lock_guard lock(mutex);
      capacity_warning = message;
    }
    waiting = true;
  }, false, 32);
  proxy.start();
  const auto connect = "CONNECT " + authority(server.port) + " HTTP/1.1\r\n\r\n";
  std::vector<std::unique_ptr<Socket>> clients;
  for (int i = 0; i < 32; ++i) {
    clients.push_back(std::make_unique<Socket>(dial(proxy.port(), i % 2 != 0)));
    IUTEST_ASSERT_TRUE(send_text(clients.back()->get(), connect));
    IUTEST_ASSERT_TRUE(read_header(clients.back()->get()).starts_with("HTTP/1.1 200")) << i;
  }
  Socket queued_v4(dial(proxy.port()));
  Socket queued_v6(dial(proxy.port(), true));
  Socket queued_denied(dial(proxy.port()));
  IUTEST_ASSERT_TRUE(send_text(queued_v4.get(), connect));
  IUTEST_ASSERT_TRUE(send_text(queued_v6.get(), connect));
  IUTEST_ASSERT_TRUE(send_text(queued_denied.get(), "CONNECT forbidden.invalid:443 HTTP/1.1\r\n\r\n"));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!waiting && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  IUTEST_ASSERT_TRUE(waiting.load());
  pollfd queued[] = {{queued_v4.get(), POLLIN, 0}, {queued_v6.get(), POLLIN, 0}, {queued_denied.get(), POLLIN, 0}};
  IUTEST_EXPECT_EQ(0, ::poll(queued, 3, 100));
  {
    std::lock_guard lock(mutex);
    IUTEST_ASSERT_EQ(32u, upstreams.size());
    upstreams[0].reset();
    upstreams[1].reset();
    upstreams[2].reset();
  }
  clients[0].reset();
  clients[1].reset();
  clients[2].reset();
  for (int fd : {queued_v4.get(), queued_v6.get()}) {
    IUTEST_EXPECT_TRUE(read_header(fd).starts_with("HTTP/1.1 200"));
    char body[4]{};
    IUTEST_EXPECT_EQ(4, ::recv(fd, body, sizeof(body), MSG_WAITALL));
    IUTEST_EXPECT_EQ("pong", std::string(body, sizeof(body)));
  }
  IUTEST_EXPECT_TRUE(read_header(queued_denied.get()).starts_with("HTTP/1.1 403"));
  proxy.stop();
  server.stop();
  std::lock_guard lock(mutex);
  IUTEST_EXPECT_NE(std::string::npos, capacity_warning.find("active 32/32"));
  IUTEST_EXPECT_NE(std::string::npos, capacity_warning.find("; at "));
}

IUTEST(ProxyTest, ConnectionLimitIsConfigurableAndValidated) {
  IUTEST_EXPECT_EQ(128, kDefaultProxyConnections);
  for (const int invalid : {0, -1, kMaxProxyConnections + 1})
    IUTEST_EXPECT_THROW(NetworkProxy({}, {}, {}, false, invalid), std::invalid_argument) << invalid;
  std::mutex mutex;
  std::vector<std::unique_ptr<Socket>> upstreams;
  LocalServer server([&](int fd) {
    std::lock_guard lock(mutex);
    upstreams.push_back(std::make_unique<Socket>(::dup(fd)));
  });
  std::atomic<int> warnings{0};
  NetworkProxy proxy({"127.0.0.1"}, {}, [&](std::string_view message) {
    if (message.find("2 active connections") != std::string_view::npos) ++warnings;
  }, false, 2);
  proxy.start();
  const auto connect = "CONNECT " + authority(server.port) + " HTTP/1.1\r\n\r\n";
  std::vector<std::unique_ptr<Socket>> clients;
  for (int i = 0; i < 2; ++i) {
    clients.push_back(std::make_unique<Socket>(dial(proxy.port())));
    IUTEST_ASSERT_TRUE(send_text(clients.back()->get(), connect));
    IUTEST_ASSERT_TRUE(read_header(clients.back()->get()).starts_with("HTTP/1.1 200"));
  }
  Socket queued(dial(proxy.port()));
  IUTEST_ASSERT_TRUE(send_text(queued.get(), connect));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (warnings == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  IUTEST_EXPECT_GT(warnings.load(), 0);
  pollfd pending{queued.get(), POLLIN, 0};
  IUTEST_EXPECT_EQ(0, ::poll(&pending, 1, 100));
  {
    std::lock_guard lock(mutex);
    upstreams[0].reset();
  }
  clients[0].reset();
  IUTEST_EXPECT_TRUE(read_header(queued.get()).starts_with("HTTP/1.1 200"));
  proxy.stop();
  server.stop();
}

IUTEST(ProxyTest, ThrowingDenialCallbackDoesNotTerminateWorker) {
  NetworkProxy proxy({}, [](const std::string&, int) { throw std::runtime_error("callback"); });
  proxy.start();
  query_proxy(proxy, "CONNECT localhost:80 HTTP/1.1\r\n\r\n");
  proxy.stop();
}
