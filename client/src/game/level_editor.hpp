#pragma once

#include "tile_layer.hpp"

#include "opm/engine.hpp"
#include "opm/level.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace opm::client::game {

enum class EditorLayer : std::uint8_t {
    Background = 0,
    Foliage = 1,
    Foreground = 2,
    Actors = 3,
};

struct LayeredEntries {
    std::vector<opm::client::render::TileDrawEntry> background {};
    std::vector<opm::client::render::TileDrawEntry> foliage {};
    std::vector<opm::client::render::TileDrawEntry> foreground {};
};

// In-progress level being edited in the LevelCreator screen. Bundles the
// level data, the per-layer draw entries, the tool palette selection,
// and the editor's transient UI state (camera, drag, hotkeys).
struct LevelEditor {
    opm::engine::LevelData level {};
    LayeredEntries entries {};
    EditorLayer activeLayer {EditorLayer::Foliage};
    std::uint16_t selectedTile {1};
    opm::engine::ActorScript selectedActorScript {opm::engine::ActorScript::MoveRandom};
    bool selectedActorDiesWhenStomped {false};
    bool selectedActorCanJumpObstacles {false};
    bool selectedActorCanJumpRandom {false};
    bool selectedActorCanFly {false};
    std::uint8_t selectedActorKind {0};
    opm::engine::ActorCategory selectedActorCategory {opm::engine::ActorCategory::Enemy};
    char nameInput[64] {"my_level"};
    std::string statusMessage {};
    float cameraX {0.0F};
    float cameraY {0.0F};
    float zoom {1.0F};
    bool placingSpawn {false};
    bool placingGoal {false};
    bool dirty {false};
    // Edge-detect for the R hotkey (rotate tile under cursor 90 deg CW).
    bool rotateKeyPrev {false};

    // Resize controls.
    int resizeWidth {60};
    int resizeHeight {16};

    // Middle-mouse drag panning.
    bool middleDragActive {false};
    double dragStartMx {0.0};
    double dragStartMy {0.0};
    float dragStartCameraX {0.0F};
    float dragStartCameraY {0.0F};
};

} // namespace opm::client::game
