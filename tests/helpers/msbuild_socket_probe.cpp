#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string expected = argv[1];
  if (expected == "rename-allow" || expected == "rename-deny") {
    if (argc != 4) return 2;
    const int result = ::rename(argv[2], argv[3]);
    const int error = errno;
    std::cout << "rename=" << result << " errno=" << error << '\n';
    return expected == "rename-allow" ? (result == 0 ? 0 : 1)
                                     : (result != 0 && (error == EPERM || error == EACCES) ? 0 : 1);
  }
  if (argc != 3) return 2;
  if (expected == "list-allow" || expected == "list-deny") {
    DIR* directory = ::opendir(argv[2]);
    const int error = errno;
    int read_error = 0;
    if (directory != nullptr) {
      errno = 0;
      while (::readdir(directory) != nullptr) {}
      read_error = errno;
      ::closedir(directory);
    }
    std::cout << "listed=" << (directory != nullptr) << " errno=" << error << " readdir=" << read_error << '\n';
    return expected == "list-allow" ? (directory != nullptr && read_error == 0 ? 0 : 1)
                                   : (directory == nullptr && (error == EPERM || error == EACCES) ? 0 : 1);
  }
  if (expected == "tcp-loopback-allow" || expected == "tcp-loopback-deny" || expected == "tcp-any-allow" ||
      expected == "tcp-any-deny" || expected == "tcp-remote-deny") {
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server == -1) return 2;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (expected == "tcp-remote-deny") {
      address.sin_port = htons(80);
      ::inet_pton(AF_INET, "192.0.2.1", &address.sin_addr);
      const int result = ::connect(server, reinterpret_cast<sockaddr*>(&address), sizeof(address));
      const int error = errno;
      ::close(server);
      std::cout << "connect=" << result << " errno=" << error << '\n';
      return result != 0 && (error == EPERM || error == EACCES) ? 0 : 1;
    }
    const bool any = expected == "tcp-any-allow" || expected == "tcp-any-deny";
    address.sin_addr.s_addr = htonl(any ? INADDR_ANY : INADDR_LOOPBACK);
    const bool bound = ::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    const int error = errno;
    bool connected = false;
    if (bound && ::listen(server, 1) == 0) {
      socklen_t length = sizeof(address);
      ::getsockname(server, reinterpret_cast<sockaddr*>(&address), &length);
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      const int client = ::socket(AF_INET, SOCK_STREAM, 0);
      connected = client != -1 && ::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
      if (client != -1) ::close(client);
    }
    ::close(server);
    std::cout << "bound=" << bound << " connected=" << connected << " errno=" << error << '\n';
    if (expected == "tcp-loopback-allow" || expected == "tcp-any-allow") return bound && connected ? 0 : 1;
    return !bound && (error == EPERM || error == EACCES) ? 0 : 1;
  }
  if (expected == "coreclr-allow" || expected == "coreclr-deny") {
    char temp[] = "/private/tmp/.coreclr.XXXXXX";
    if (::mkdtemp(temp) == nullptr) {
      const int error = errno;
      std::cout << "mkdtemp=0 errno=" << error << '\n';
      return expected == "coreclr-deny" && (error == EPERM || error == EACCES) ? 0 : 1;
    }
    const bool moded = ::chmod(temp, 0777) == 0;
    const int mode_error = errno;
    const bool renamed = moded && ::rename(temp, argv[2]) == 0;
    const int rename_error = errno;
    if (!renamed) ::rmdir(temp);
    std::cout << "mkdtemp=1 chmod=" << moded << " errno=" << mode_error << " rename=" << renamed
              << " errno=" << rename_error << '\n';
    return expected == "coreclr-allow" && moded && renamed ? 0 : 1;
  }
  if (expected == "mkdir-allow" || expected == "mkdir-deny") {
    const int result = ::mkdir(argv[2], 0777);
    const int error = errno;
    const bool removed = result == 0 && ::rmdir(argv[2]) == 0;
    std::cout << "mkdir=" << result << " errno=" << error << " removed=" << removed << '\n';
    if (expected == "mkdir-allow") return removed ? 0 : 1;
    return result != 0 && (error == EPERM || error == EACCES) ? 0 : 1;
  }
  if (expected == "fifo-allow" || expected == "fifo-deny") {
    const int result = ::mkfifo(argv[2], 0600);
    const int error = errno;
    const bool removed = result == 0 && ::unlink(argv[2]) == 0;
    std::cout << "mkfifo=" << result << " errno=" << error << " removed=" << removed << '\n';
    if (expected == "fifo-allow") return removed ? 0 : 1;
    return result != 0 && (error == EPERM || error == EACCES) ? 0 : 1;
  }
  if (expected == "read-allow") {
    const int fd = ::open(argv[2], O_RDONLY);
    const int error = errno;
    if (fd >= 0) ::close(fd);
    std::cout << "read=" << fd << " errno=" << error << '\n';
    return fd >= 0 ? 0 : 1;
  }
  if (expected == "read-deny") {
    const int fd = ::open(argv[2], O_RDONLY);
    const int error = errno;
    if (fd >= 0) ::close(fd);
    std::cout << "read=" << fd << " errno=" << error << '\n';
    return fd < 0 && (error == EPERM || error == EACCES) ? 0 : 1;
  }
  if (expected == "modify-allow" || expected == "modify-deny") {
    const int fd = ::open(argv[2], O_WRONLY | O_APPEND);
    const int open_error = errno;
    ssize_t written = -1;
    int write_error = 0;
    if (fd >= 0) {
      written = ::write(fd, "x", 1);
      write_error = errno;
      ::close(fd);
    }
    const bool denied = (fd < 0 && (open_error == EPERM || open_error == EACCES)) ||
                        (fd >= 0 && written < 0 && (write_error == EPERM || write_error == EACCES));
    std::cout << "open=" << fd << " errno=" << open_error << " written=" << written
              << " write_errno=" << write_error << '\n';
    return expected == "modify-allow" ? (written == 1 ? 0 : 1) : (denied ? 0 : 1);
  }
  if (expected == "file-allow" || expected == "file-deny") {
    const int fd = ::open(argv[2], O_CREAT | O_EXCL | O_WRONLY, 0600);
    const int error = errno;
    if (fd != -1) {
      ::close(fd);
      if (::unlink(argv[2]) != 0) return 1;
    }
    std::cout << "open=" << fd << " errno=" << error << '\n';
    return expected == "file-allow" ? (fd != -1 ? 0 : 1)
                                    : (fd == -1 && (error == EPERM || error == EACCES) ? 0 : 1);
  }
  if (expected == "connect-allow" || expected == "connect-deny") {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = argv[2];
    if (path.size() >= sizeof(address.sun_path)) return 2;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket == -1) return 2;
    const int result = ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const int error = errno;
    ::close(socket);
    std::cout << "connect=" << result << " errno=" << error << '\n';
    return expected == "connect-allow" ? (result == 0 ? 0 : 1)
                                       : (result != 0 && (error == EPERM || error == EACCES) ? 0 : 1);
  }
  const bool unity = expected == "unity-allow" || expected == "unity-deny";
  const bool ilpp = expected == "ilpp-allow" || expected == "ilpp-deny";
  const bool explicit_path = expected == "socket-allow" || expected == "socket-deny";
  if (!unity && !ilpp && !explicit_path && expected != "allow" && expected != "deny") return 2;
  const bool allow = expected == "allow" || expected == "unity-allow" || expected == "ilpp-allow" ||
                     expected == "socket-allow";
  std::ostringstream suffix;
  suffix << std::hex << std::setfill('0') << std::setw(32) << ::getpid();
  const std::string path = explicit_path ? argv[2] : (ilpp ? "/private/tmp/ilpp.sock-" + suffix.str()
                               : (unity ? "/private/tmp/Unity-Upm-" : "/private/tmp/MSBuild") +
                                     std::to_string(::getpid()) + (unity ? ".sock" : "")) + argv[2];
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) return 2;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (server == -1) return 2;
  const bool bound = ::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  const int error = errno;
  bool connected = false;
  if (bound && ::listen(server, 1) == 0) {
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    connected = client != -1 && ::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    if (client != -1) ::close(client);
  }
  ::close(server);
  if (bound) ::unlink(path.c_str());
  std::cout << "bound=" << bound << " connected=" << connected << " errno=" << error << '\n';
  return allow ? (bound && connected ? 0 : 1)
                             : (!bound && (error == EPERM || error == EACCES) ? 0 : 1);
}
