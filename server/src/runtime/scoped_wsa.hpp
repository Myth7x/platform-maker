#pragma once
#include "net/socket_compat.hpp"

namespace opm::server {

// RAII WSAStartup / WSACleanup for Windows; no-op on POSIX. Header-only because
// the Windows headers are already pulled in by socket_compat.hpp.
class ScopedWsa {
public:
    ScopedWsa() noexcept
    {
#ifdef _WIN32
        WSADATA data;
        ok_ = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
#else
        ok_ = true;
#endif
    }
    ~ScopedWsa()
    {
#ifdef _WIN32
        if (ok_) {
            ::WSACleanup();
        }
#endif
    }
    ScopedWsa(const ScopedWsa&) = delete;
    ScopedWsa& operator=(const ScopedWsa&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ {false};
};

} // namespace opm::server
