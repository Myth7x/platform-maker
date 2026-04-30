#include "server_app.hpp"

#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    std::uint16_t port = 34900;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && (i + 1) < argc) {
            port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
        }
    }

    std::cout << "open-platformer-maker server bootstrap\n";
    opm::server::ServerApp app(port);
    return app.run();
}
