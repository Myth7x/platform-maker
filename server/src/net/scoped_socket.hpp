#pragma once
#include "net/socket_compat.hpp"

namespace opm::server {

// Move-only RAII wrapper around a non-blocking socket handle.
class ScopedSocket {
public:
    ScopedSocket() noexcept = default;
    explicit ScopedSocket(socket_t fd) noexcept : fd_(fd) {}
    ~ScopedSocket() { close(); }

    ScopedSocket(ScopedSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidSocket; }
    ScopedSocket& operator=(ScopedSocket&& other) noexcept
    {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = kInvalidSocket;
        }
        return *this;
    }
    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;

    void close() noexcept
    {
        if (fd_ != kInvalidSocket) {
            closesocketCompat(fd_);
            fd_ = kInvalidSocket;
        }
    }
    [[nodiscard]] socket_t handle() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ != kInvalidSocket; }
    socket_t release() noexcept
    {
        const auto h = fd_;
        fd_ = kInvalidSocket;
        return h;
    }

private:
    socket_t fd_ {kInvalidSocket};
};

} // namespace opm::server
