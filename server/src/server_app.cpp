#include "server_app.hpp"

#include "server.hpp"

namespace opm::server {

ServerApp::ServerApp(const std::uint16_t port)
    : port_(port)
{
}

int ServerApp::run()
{
    Server server(port_);
    return server.run();
}

} // namespace opm::server
