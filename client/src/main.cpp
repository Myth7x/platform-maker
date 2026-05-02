#include "client_app.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    std::cout << "open-platformer-maker client bootstrap\n";

    opm::client::ClientArgs args;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--test-mode") {
            args.testMode = true;
        } else if (arg == "--test-address" && i + 1 < argc) {
            args.testAddress = argv[++i];
        } else if (arg == "--test-level" && i + 1 < argc) {
            args.testLevel = argv[++i];
        } else {
            std::cerr << "[client] unknown argument: " << arg << "\n";
        }
    }

    opm::client::ClientApp app;
    return app.run(args);
}
