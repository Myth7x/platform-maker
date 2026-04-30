#include "client_app.hpp"

#include <iostream>

int main()
{
    std::cout << "open-platformer-maker client bootstrap\n";
    opm::client::ClientApp app;
    return app.run();
}
