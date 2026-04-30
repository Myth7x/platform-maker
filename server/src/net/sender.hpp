#pragma once
#include "net/socket_compat.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace opm::server {

// Non-blocking-aware send loop. Tolerates transient WOULDBLOCK with bounded
// retry; returns false on hard failure or repeated backpressure.
[[nodiscard]] bool sendAll(socket_t fd, std::span<const std::uint8_t> data);

} // namespace opm::server
