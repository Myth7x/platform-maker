#pragma once

#include <cstdint>

namespace opm::server {

class ServerApp {
public:
    explicit ServerApp(std::uint16_t port);
    int run();

private:
    std::uint16_t port_;
};

} // namespace opm::server
