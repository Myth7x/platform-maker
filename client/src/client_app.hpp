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
// work via ScreenStack. runWindow is the platform-specific entry that
// owns GLFW/ImGui setup, the per-frame loop, and the per-state render /
// input branches that haven't yet migrated into Screen subclasses.
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
