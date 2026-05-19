#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace opm::server {

#ifdef _WIN32
using socket_t = SOCKET;
using ssize_t = long long;
using pollfd_t = WSAPOLLFD;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int closesocketCompat(socket_t s) noexcept { return ::closesocket(s); }
inline bool sockWouldBlock() noexcept { return ::WSAGetLastError() == WSAEWOULDBLOCK; }
inline int sockPoll(pollfd_t* fds, ULONG nfds, INT timeoutMs) noexcept { return ::WSAPoll(fds, nfds, timeoutMs); }
#else
using socket_t = int;
using pollfd_t = ::pollfd;
inline constexpr socket_t kInvalidSocket = -1;
inline int closesocketCompat(socket_t s) noexcept { return ::close(s); }
inline bool sockWouldBlock() noexcept { return errno == EAGAIN || errno == EWOULDBLOCK; }
inline int sockPoll(pollfd_t* fds, nfds_t nfds, int timeoutMs) noexcept { return ::poll(fds, nfds, timeoutMs); }
#endif

[[nodiscard]] inline bool setSocketNonBlocking(socket_t fd) noexcept
{
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

inline void enableTcpNoDelay(socket_t fd) noexcept
{
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
}

} // namespace opm::server
