#pragma once

#include <string>

namespace opm::assets { struct AssetManifest; }
namespace opm::engine { struct LevelData; }

namespace opm::client {

// Optional arguments parsed from the command line.
struct ClientArgs {
    // --test-mode: automatically connect and play on the specified level.
    bool testMode {false};
    std::string testAddress {"127.0.0.1:34900"};
    std::string testLevel {"test_room"}; // level name on the server
};

// Top-level orchestrator. Mirrors the server's `Server` class: owns the
// long-lived subsystems, drives the main loop, and dispatches per-screen
// work. Currently a thin wrapper around the legacy runWindow body; the
// per-screen carve-out (Step 4 cont.) will move work out into Screen
// classes invoked through ScreenStack.
class ClientApp {
public:
    int run();
    int run(const ClientArgs& args);

private:
    int runWindow(const opm::assets::AssetManifest& manifest,
                  const opm::engine::LevelData& fallbackLevel,
                  const ClientArgs& clientArgs);
};

} // namespace opm::client
