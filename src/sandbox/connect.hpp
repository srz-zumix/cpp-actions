#pragma once

#include <netdb.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <vector>

namespace cpa::sandbox::detail {

bool public_addresses(const addrinfo* first);

inline constexpr auto kDnsTimeout = std::chrono::seconds(15);
inline constexpr auto kTcpTimeout = std::chrono::seconds(15);
inline constexpr auto kConnectionDelay = std::chrono::milliseconds(250);

inline std::vector<const addrinfo*> interleave_addresses(const addrinfo* first) {
  std::vector<const addrinfo*> preferred;
  std::vector<const addrinfo*> alternate;
  for (auto* address = first; address; address = address->ai_next) {
    (address->ai_family == first->ai_family ? preferred : alternate).push_back(address);
  }
  std::vector<const addrinfo*> result;
  result.reserve(preferred.size() + alternate.size());
  for (std::size_t i = 0; i < std::max(preferred.size(), alternate.size()); ++i) {
    if (i < preferred.size()) result.push_back(preferred[i]);
    if (i < alternate.size()) result.push_back(alternate[i]);
  }
  return result;
}

struct ConnectionAttempt {
  int fd;
  int error;
};

struct PendingConnection {
  int fd;
  int error;
  std::size_t address;
};

// The connector owns socket operations and the clock so scheduling can be tested
// without relying on an unreachable host or platform-specific TCP retransmits.
template <class Connector>
int race_connections(std::size_t count, Connector& connector) {
  struct Pending {
    Connector& connector;
    std::vector<PendingConnection> sockets;
    ~Pending() {
      for (const auto& socket : sockets)
        if (socket.fd >= 0) connector.close(socket.fd);
    }
  } pending{connector, {}};
  pending.sockets.reserve(count);
  const auto deadline = connector.now() + kTcpTimeout;
  auto next_launch = connector.now();
  std::size_t next = 0;
  while (!connector.stopped()) {
    for (auto it = pending.sockets.begin(); it != pending.sockets.end();) {
      if (it->error == EINPROGRESS) {
        ++it;
        continue;
      }
      if (it->error == 0) it->error = connector.finish(it->fd);
      if (it->error == 0 && !connector.stopped()) {
        const int winner = it->fd;
        it->fd = -1;
        return winner;
      }
      if (connector.stopped()) return -1;
      connector.record(it->address, it->error);
      if (it->fd >= 0) connector.close(it->fd);
      it = pending.sockets.erase(it);
      next_launch = connector.now();
    }
    const auto now = connector.now();
    if (now >= deadline) break;
    if (next < count && (pending.sockets.empty() || now >= next_launch)) {
      const auto attempt = connector.start(next);
      pending.sockets.push_back({attempt.fd, attempt.error, next++});
      next_launch = now + kConnectionDelay;
      continue;
    }
    if (pending.sockets.empty()) return -1;
    auto wait = std::min(std::chrono::milliseconds(50),
                         std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
    if (next < count)
      wait = std::min(wait, std::chrono::duration_cast<std::chrono::milliseconds>(next_launch - now));
    connector.wait(pending.sockets, std::max(wait, std::chrono::milliseconds(1)));
  }
  if (!connector.stopped()) {
    for (const auto& socket : pending.sockets) connector.record(socket.address, ETIMEDOUT);
  }
  return -1;
}

}  // namespace cpa::sandbox::detail
