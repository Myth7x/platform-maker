#include "client_app.hpp"

#include "game/actor_manager.hpp"
#include "game/game_session.hpp"
#include "game/level_editor.hpp"
#include "game/network_session.hpp"
#include "net_client.hpp"
#include "render/asset_registry.hpp"
#include "render/hud.hpp"
#include "render/render_context.hpp"
#include "render/sprite.hpp"
#include "render/texture.hpp"
#include "render/texture_loader.hpp"
#include "screens/level_creator_screen.hpp"
#include "screens/level_picker_screen.hpp"
#include "screens/main_menu_screen.hpp"
#include "screens/online_level_select_screen.hpp"
#include "screens/playing_screen.hpp"
#include "sprite_config.hpp"
#include "tile_layer.hpp"

#include "opm/assets.hpp"
#include "opm/engine.hpp"
#include "opm/level.hpp"
#include "opm/protocol.hpp"

#include <cstdio>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)
#ifdef OPM_CLIENT_WITH_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#endif

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
#include <GL/gl.h>
// GL_CLAMP_TO_EDGE is OpenGL 1.2 but missing from the Win32 gl.h header.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl2.h>
#endif

namespace opm::client {
namespace {

constexpr float kPixelsPerTile = 24.0F;
constexpr float kFixedStepSeconds = 1.0F / 60.0F;
constexpr float kPlayerRenderWidthTiles = 40.0F / kPixelsPerTile;
constexpr float kPlayerRenderHeightTiles = 40.0F / kPixelsPerTile;

using opm::client::game::kInvalidServerIndex;
using opm::client::game::levelFromSnapshot;
using opm::client::game::NetworkSessionContext;
using opm::client::game::parseAddress;
using opm::client::game::tryConnect;
using opm::client::game::tryLobbyFlow;

NetworkSessionContext gNetwork {};

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
using opm::client::render::AssetRegistry;
using opm::client::render::EnemyRegistry;
using opm::client::render::EnemySprite;
using opm::client::render::RenderContext;
using opm::client::render::Texture2D;
using opm::client::render::drawTextureQuad;
#endif

opm::assets::AssetManifest loadAndPrintAssetSummary(const std::filesystem::path& root)
{
    const auto manifest = opm::assets::buildManifest(root);
    std::uint32_t bgCount = 0;
    std::uint32_t tilesCount = 0;
    std::uint32_t objectCount = 0;

    for (const auto& record : manifest.records) {
        if (record.category == "BG") {
            bgCount += 1;
        } else if (record.category == "Tiles") {
            tilesCount += 1;
        } else if (record.category == "Object") {
            objectCount += 1;
        }
    }

    std::cout << "[client] assets root: " << root.string() << "\n";
    std::cout << "[client] BG png count: " << bgCount << "\n";
    std::cout << "[client] Tiles png count: " << tilesCount << "\n";
    std::cout << "[client] Object png count: " << objectCount << "\n";
    if (!manifest.records.empty()) {
        std::cout << "[client] first asset id: " << manifest.records.front().id << "\n";
    }

    return manifest;
}

void printTileLayerStubSummary(const opm::assets::AssetManifest& manifest, const opm::engine::LevelData& level)
{
    const auto entries = opm::client::render::buildTileLayerDrawEntries(manifest, level.foliage, 24.0F);
    std::cout << "[client] tile layer draw entry count: " << entries.size() << "\n";
    if (!entries.empty()) {
        const auto& first = entries.front();
        std::cout << "[client] first tile draw entry: id=" << first.tileAssetId
                  << " tile=(" << first.tileX << "," << first.tileY << ")"
                  << " world=(" << first.worldX << "," << first.worldY << ")"
                  << " size=" << first.tileSize << "\n";
    }
}


} // namespace

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)

using opm::client::game::AppState;
using opm::client::game::EditorLayer;
using opm::client::game::GameSession;
using opm::client::game::LayeredEntries;
using opm::client::game::LevelEditor;


int ClientApp::runWindow(const opm::assets::AssetManifest& manifest, const opm::engine::LevelData& fallbackLevel,
                    const ClientArgs& clientArgs)
{
    RenderContext renderCtx(800, 600, "Open Platformer Maker");
    if (!renderCtx.ok()) {
        return 1;
    }
    GLFWwindow* window = renderCtx.window();

#ifdef OPM_CLIENT_WITH_VULKAN
    if (glfwVulkanSupported() == GLFW_TRUE) {
        std::uint32_t apiVersion = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion(&apiVersion) == VK_SUCCESS) {
            std::cout << "[client] Vulkan API version: "
                      << VK_API_VERSION_MAJOR(apiVersion) << "."
                      << VK_API_VERSION_MINOR(apiVersion) << "."
                      << VK_API_VERSION_PATCH(apiVersion) << "\n";
        }
    }
#endif

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
    AssetRegistry assets;
    assets.load(manifest, OPM_CLIENT_RESOURCE_DIR);
    auto& textures        = assets.tileTextures;
    auto& playerSpriteSet = assets.playerSprites;
    auto& enemyRegistry   = assets.enemies;
    auto& powerupRegistry = assets.powerups;
    auto& palette         = assets.palette;
#endif

    // session, manifest are members of ClientApp; alias them locally so
    // the remaining code in this function (~1100 lines of fixed-step,
    // render, and input branches) keeps using bare `session.X` /
    // `manifest` names without churn.
    manifest_ = &manifest;
    auto& session = session_;

    // Helpers that used to be local lambdas are now ClientApp methods.
    // Re-bind as lambdas at the call sites below where capture syntax
    // is most natural; here we just bind a couple that take palette.
    auto enterLevelCreator = [this, &palette]() { this->enterLevelCreator(palette); };
    auto editLoadedLevel = [this, &palette](opm::engine::LevelData loaded, const std::string& name) {
        this->editLoadedLevel(std::move(loaded), name, palette);
    };

    // Per-screen handlers. MainMenuScreen is the first one carved out
    // of runWindow's monolithic ImGui block (Step 4 cont.). The others
    // still live as inline branches below until they're migrated.
    MainMenuScreen mainMenu(session, MainMenuScreen::Callbacks {
        .onEnterLevelPicker = [&](const std::string& host, std::uint16_t port,
                                  GameSession::PickerIntent intent) {
            return enterLevelPicker(host, port, intent);
        },
        .onPlayQuickOffline = [&]() {
            gNetwork.connected = false;
            gNetwork.localPlayerIndex = kInvalidServerIndex;
            gNetwork.actors.resetLocalOnly();
            enterPlaying(false, fallbackLevel);
        },
        .onPlayOnline = [&](const std::string& host, std::uint16_t port) -> std::string {
            const auto result = tryLobbyFlow(gNetwork, host, port);
            if (!result.ok) {
                return result.message;
            }
            std::string status;
            session.onlineLevels.clear();
            session.onlineLevelSelected = -1;
            session.onlineLevelStatus.clear();
            if (gNetwork.session) {
                (void)gNetwork.session->requestLevelList(2000U, session.onlineLevels, status);
            }
            session.state = AppState::OnlineLevelSelect;
            return {};
        },
        .onEnterLevelCreator = [&]() {
            enterLevelCreator();
        },
        .onQuit = [&]() {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        },
    });

    LevelPickerScreen levelPicker(session, LevelPickerScreen::Callbacks {
        .onEditLoadedLevel = [&](opm::engine::LevelData loaded, const std::string& name) {
            editLoadedLevel(std::move(loaded), name);
        },
        .onPlayLoadedLevelOffline = [&](opm::engine::LevelData loaded) {
            gNetwork.connected = false;
            gNetwork.localPlayerIndex = kInvalidServerIndex;
            gNetwork.actors.resetLocalOnly();
            enterPlaying(false, loaded);
        },
    });

    OnlineLevelSelectScreen onlineLevelSelect(session, OnlineLevelSelectScreen::Callbacks {
        .onPollServer = [&]() {
            if (!gNetwork.session || !gNetwork.session->isConnected()) {
                return;
            }
            std::string discardStatus;
            opm::client::net::StateUpdate discard;
            while (gNetwork.session->pollStateUpdate(0U, discard, discardStatus)) {
                // Discard — gameplay hasn't started.
            }
            std::vector<opm::client::net::LevelSnapshot> snaps;
            gNetwork.session->drainLevelSnapshots(snaps);
            for (auto& s : snaps) {
                gNetwork.networkLevel = levelFromSnapshot(s);
            }
            std::vector<std::vector<opm::protocol::PlayerInfo>> rosterUpdates;
            gNetwork.session->drainRosterUpdates(rosterUpdates);
            for (const auto& roster : rosterUpdates) {
                gNetwork.actors.applyRoster(roster, gNetwork.localPlayerIndex);
            }
            gNetwork.session->sendPingIfDue(1000U);
        },
        .getLocalPlayerIndex = [&]() {
            return static_cast<int>(gNetwork.localPlayerIndex);
        },
        .onUseSelectedLevel = [&](const std::string& name) -> std::string {
            std::string status;
            if (!gNetwork.session || !gNetwork.session->requestSetLobbyLevel(name, 2000U, status)) {
                return "set level failed: " + status;
            }
            // Server broadcasts a fresh LevelSnapshot right after the
            // response. Drain a few state updates briefly to give the
            // snapshot time to arrive before we enter Playing.
            std::vector<opm::client::net::LevelSnapshot> snaps;
            gNetwork.session->drainLevelSnapshots(snaps);
            for (int i = 0; i < 10 && snaps.empty(); ++i) {
                opm::client::net::StateUpdate discard;
                std::string s;
                (void)gNetwork.session->pollStateUpdate(20U, discard, s);
                gNetwork.session->drainLevelSnapshots(snaps);
            }
            for (auto& s : snaps) {
                gNetwork.networkLevel = levelFromSnapshot(s);
            }
            enterPlaying(true, gNetwork.networkLevel);
            return {};
        },
        .onUseCurrentLevel = [&]() {
            enterPlaying(true, gNetwork.networkLevel);
        },
        .onRefresh = [&]() -> std::string {
            std::string status;
            if (!gNetwork.session ||
                !gNetwork.session->requestLevelList(2000U, session.onlineLevels, status)) {
                return "refresh failed: " + status;
            }
            return {};
        },
        .onDisconnect = [&]() {
            if (gNetwork.session) {
                gNetwork.session->disconnect();
            }
            gNetwork.connected = false;
            gNetwork.localPlayerIndex = kInvalidServerIndex;
            gNetwork.actors.resetLocalOnly();
            session.state = AppState::MainMenu;
        },
    });

    PlayingScreen playing(session);

    LevelCreatorScreen levelCreator(session, LevelCreatorScreen::Callbacks {
        .onSaveLevel = [&]() {
            std::string host;
            std::uint16_t port = 0;
            if (!parseAddress(session.addressInput, host, port)) {
                session.editor.statusMessage = "Invalid server address.";
            } else if (!gNetwork.session || !gNetwork.session->isConnected()) {
                const auto cr = tryConnect(gNetwork, host, port);
                if (!cr.ok) {
                    session.editor.statusMessage = cr.message;
                }
            }
            if (gNetwork.session && gNetwork.session->isConnected()) {
                std::string status;
                if (gNetwork.session->requestSaveLevel(session.editor.nameInput, session.editor.level, 2000U, status)) {
                    session.editor.statusMessage = "Saved.";
                    session.editor.dirty = false;
                } else {
                    session.editor.statusMessage = "Save failed: " + status;
                }
            }
        },
        .onTestPlay = [&]() {
            gNetwork.connected = false;
            gNetwork.localPlayerIndex = kInvalidServerIndex;
            gNetwork.actors.resetLocalOnly();
            enterPlaying(false, session.editor.level);
            session.fromEditor = true;
        },
        .onResize = [&](int w, int h) {
            const auto maxDim = static_cast<int>(opm::engine::kMaxLevelDimension);
            w = std::clamp(w, 1, maxDim);
            h = std::clamp(h, 1, maxDim);
            session.editor.resizeWidth = w;
            session.editor.resizeHeight = h;
            opm::engine::resizeAllLayers(session.editor.level,
                static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
            rebuildEditorEntries();
            session.editor.dirty = true;
            session.editor.statusMessage = "Resized to " +
                std::to_string(w) + "x" + std::to_string(h);
        },
    });

    bool jumpHeldLast = false;
    double previousTime = glfwGetTime();
    double accumulator = 0.0;
    float animationTime = 0.0F;

    // ---- Test-mode auto-join ----
    // When launched with --test-mode, skip the main menu entirely:
    // connect, join the first lobby, set the requested level, then start playing.
    if (clientArgs.testMode) {
        std::string host;
        std::uint16_t port = 0;
        if (parseAddress(clientArgs.testAddress, host, port)) {
            std::snprintf(session.addressInput, sizeof(session.addressInput), "%s", clientArgs.testAddress.c_str());
            const auto result = tryLobbyFlow(gNetwork, host, port);
            if (result.ok && gNetwork.session) {
                // Try to set the requested level.
                std::string status;
                std::string targetLevel = clientArgs.testLevel;

                // Fetch the level list to verify the level exists.
                std::vector<std::string> levels;
                if (gNetwork.session->requestLevelList(2000U, levels, status)) {
                    const auto it = std::find(levels.begin(), levels.end(), targetLevel);
                    if (it != levels.end()) {
                        if (gNetwork.session->requestSetLobbyLevel(targetLevel, 2000U, status)) {
                            // Drain snapshots so we get the updated level.
                            std::vector<opm::client::net::LevelSnapshot> snaps;
                            gNetwork.session->drainLevelSnapshots(snaps);
                            for (int i = 0; i < 10 && snaps.empty(); ++i) {
                                opm::client::net::StateUpdate discard;
                                std::string s;
                                (void)gNetwork.session->pollStateUpdate(20U, discard, s);
                                gNetwork.session->drainLevelSnapshots(snaps);
                            }
                            for (auto& s : snaps) {
                                gNetwork.networkLevel = levelFromSnapshot(s);
                            }
                            std::cout << "[test-mode] level set to '" << targetLevel << "'\n";
                        } else {
                            std::cout << "[test-mode] set level failed: " << status << " — using current level\n";
                        }
                    } else {
                        std::cout << "[test-mode] level '" << targetLevel << "' not found on server"
                                  << " — available: ";
                        for (const auto& l : levels) { std::cout << l << " "; }
                        std::cout << "\n";
                    }
                }
                enterPlaying(true, gNetwork.networkLevel);
                std::cout << "[test-mode] auto-joined online play\n";
            } else {
                std::cout << "[test-mode] connection failed: " << result.message << "\n";
            }
        } else {
            std::cout << "[test-mode] invalid address: " << clientArgs.testAddress << "\n";
        }
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

#ifdef OPM_CLIENT_HAS_IMGUI
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
#endif

        // ---- Tick gameplay (fixed-step, only while playing) ----
        if (session.state == AppState::Playing) {
            const double now = glfwGetTime();
            double frameTime = now - previousTime;
            previousTime = now;
            if (frameTime > 0.1) {
                frameTime = 0.1;
            }
            accumulator += frameTime;

            while (accumulator >= kFixedStepSeconds) {
#ifdef OPM_CLIENT_HAS_IMGUI
                const bool imguiCapturesKeyboard = ImGui::GetIO().WantCaptureKeyboard;
#else
                const bool imguiCapturesKeyboard = false;
#endif
                const bool moveLeft = !imguiCapturesKeyboard &&
                    (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ||
                     glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS);
                const bool moveRight = !imguiCapturesKeyboard &&
                    (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ||
                     glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS);
                const bool jumpHeld = !imguiCapturesKeyboard &&
                    (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS ||
                     glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS);
                const bool runHeld = !imguiCapturesKeyboard &&
                    (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                     glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
                const bool crouchHeld = !imguiCapturesKeyboard &&
                    (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ||
                     glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS);

                if (session.isOnline && gNetwork.session) {
                    std::string netStatus;

                    opm::client::net::StateUpdate update;
                    bool gotTick = false;
                    while (gNetwork.session->pollStateUpdate(0U, update, netStatus)) {
                        gNetwork.actors.applyStateUpdate(update, gNetwork.localPlayerIndex);
                        gotTick = true;
                    }

                    std::vector<std::vector<opm::protocol::PlayerInfo>> rosterUpdates;
                    gNetwork.session->drainRosterUpdates(rosterUpdates);
                    for (const auto& roster : rosterUpdates) {
                        gNetwork.actors.applyRoster(roster, gNetwork.localPlayerIndex);
                    }

                    std::vector<opm::client::net::LevelSnapshot> snaps;
                    gNetwork.session->drainLevelSnapshots(snaps);
                    if (!snaps.empty()) {
                        const auto newLevel = levelFromSnapshot(snaps.back());
                        gNetwork.networkLevel = newLevel;
                        session.activeLevel = newLevel;
                        rebuildEntriesFromLevel(session.entries, newLevel);
                        session.simulation.setLevel(newLevel);
                        opm::engine::PlayerState localSpawnState;
                        localSpawnState.position.x = newLevel.spawnX;
                        localSpawnState.position.y = newLevel.spawnY;
                        gNetwork.actors.updateLocalState(localSpawnState);
                        std::cout << "[client] level switched mid-session: "
                                  << newLevel.foliage.width << "x" << newLevel.foliage.height << "\n";
                    }

                    gNetwork.session->sendPingIfDue(500U);

                    if (gotTick) {
                        opm::engine::InputFrame networkInput {};
                        networkInput.frameIndex = gNetwork.actors.lastServerTick();
                        networkInput.moveLeft = moveLeft;
                        networkInput.moveRight = moveRight;
                        networkInput.jumpPressed = jumpHeld && !jumpHeldLast;
                        networkInput.jumpHeld = jumpHeld;
                        networkInput.runHeld = runHeld;
                        networkInput.crouchHeld = crouchHeld;
                        (void)gNetwork.session->sendMovementInput(networkInput, netStatus);
                    }
                } else {
                    // Offline single-player: run local simulation.
                    std::array<opm::engine::InputFrame, 2> inputs {};
                    inputs[0].frameIndex = session.simulation.state().tick;
                    inputs[0].moveLeft = moveLeft;
                    inputs[0].moveRight = moveRight;
                    inputs[0].jumpPressed = jumpHeld && !jumpHeldLast;
                    inputs[0].jumpHeld = jumpHeld;
                    inputs[0].runHeld = runHeld;
                    inputs[0].crouchHeld = crouchHeld;
                    inputs[1].frameIndex = session.simulation.state().tick;
                    session.simulation.step(inputs);
                    gNetwork.actors.updateLocalState(session.simulation.state().players[0]);
                    gNetwork.actors.setWorldActors(session.simulation.state().actors);
                }

                jumpHeldLast = jumpHeld;
                animationTime += kFixedStepSeconds;
                accumulator -= kFixedStepSeconds;
            }
        } else {
            // Reset frame timing while in menu so we don't burst-step on entry.
            previousTime = glfwGetTime();
            accumulator = 0.0;
            jumpHeldLast = false;
        }

        // ---- Render ----
#ifdef OPM_CLIENT_WITH_OPENGL_STUB
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
#ifdef OPM_CLIENT_HAS_IMGUI
            ImGui::EndFrame();
#endif
            continue;
        }

        glViewport(0, 0, framebufferWidth, framebufferHeight);
        glClearColor(0.10F, 0.12F, 0.16F, 1.0F);
        if (session.state == AppState::Playing) {
            glClearColor(0.53F, 0.75F, 0.92F, 1.0F);
        } else if (session.state == AppState::LevelCreator) {
            glClearColor(0.18F, 0.20F, 0.26F, 1.0F);
        }
        glClear(GL_COLOR_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(framebufferWidth), 0.0, static_cast<double>(framebufferHeight), -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        if (session.state == AppState::Playing) {
            ScreenContext screenCtx { nullptr, renderCtx, assets, gNetwork.session.get(),
                &gNetwork, framebufferWidth, framebufferHeight, animationTime };
            playing.render(screenCtx);
        } else if (session.state == AppState::LevelCreator) {
#ifdef OPM_CLIENT_HAS_IMGUI
            const bool kbCaptured = ImGui::GetIO().WantCaptureKeyboard;
            const bool mouseCapturedForPan = ImGui::GetIO().WantCaptureMouse;
#else
            const bool kbCaptured = false;
            const bool mouseCapturedForPan = false;
#endif
            const float editorPixelsPerTile = kPixelsPerTile * session.editor.zoom;
            const float zoomScale = (kPixelsPerTile > 0.0F) ? (editorPixelsPerTile / kPixelsPerTile) : 1.0F;

            // Mouse wheel zoom, anchored at cursor position over the canvas.
            // Keeping the world point under cursor stable makes zoom feel natural.
#ifdef OPM_CLIENT_HAS_IMGUI
            if (!mouseCapturedForPan) {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0F) {
                    int winW = 0;
                    int winH = 0;
                    glfwGetWindowSize(window, &winW, &winH);
                    if (winW > 0 && winH > 0) {
                        double mx = 0.0;
                        double my = 0.0;
                        glfwGetCursorPos(window, &mx, &my);
                        const float cursorPx = static_cast<float>(mx) * (static_cast<float>(framebufferWidth) / static_cast<float>(winW));
                        const float cursorPy = static_cast<float>(framebufferHeight) -
                            static_cast<float>(my) * (static_cast<float>(framebufferHeight) / static_cast<float>(winH));

                        const float oldPixelsPerTile = editorPixelsPerTile;
                        const float worldTileX = (cursorPx + session.editor.cameraX) / oldPixelsPerTile;
                        const float worldTileY = (cursorPy + session.editor.cameraY) / oldPixelsPerTile;

                        const float zoomFactor = (wheel > 0.0F) ? 1.1F : 0.9F;
                        session.editor.zoom = std::clamp(session.editor.zoom * zoomFactor, 0.5F, 4.0F);

                        const float newPixelsPerTile = kPixelsPerTile * session.editor.zoom;
                        session.editor.cameraX = worldTileX * newPixelsPerTile - cursorPx;
                        session.editor.cameraY = worldTileY * newPixelsPerTile - cursorPy;
                    }
                }
            }
#endif

            // Camera pan via arrow keys / A-D (when not editing the name field).
            if (!kbCaptured) {
                const float panSpeed = 8.0F;
                if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS ||
                    glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
                    session.editor.cameraX -= panSpeed;
                }
                if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS ||
                    glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
                    session.editor.cameraX += panSpeed;
                }
                if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS ||
                    glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
                    session.editor.cameraY -= panSpeed;
                }
                if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS ||
                    glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
                    session.editor.cameraY += panSpeed;
                }
            }

            // Middle-mouse drag pan: start when MMB pressed over canvas, then
            // keep tracking until released (even if cursor crosses ImGui panels).
            const bool mmbDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
            int winW = 0, winH = 0;
            glfwGetWindowSize(window, &winW, &winH);
            const float pxScale = (winW > 0) ? (static_cast<float>(framebufferWidth) / static_cast<float>(winW)) : 1.0F;
            if (mmbDown) {
                if (!session.editor.middleDragActive && !mouseCapturedForPan) {
                    double mx = 0.0, my = 0.0;
                    glfwGetCursorPos(window, &mx, &my);
                    session.editor.middleDragActive = true;
                    session.editor.dragStartMx = mx;
                    session.editor.dragStartMy = my;
                    session.editor.dragStartCameraX = session.editor.cameraX;
                    session.editor.dragStartCameraY = session.editor.cameraY;
                } else if (session.editor.middleDragActive) {
                    double mx = 0.0, my = 0.0;
                    glfwGetCursorPos(window, &mx, &my);
                    const float dx = static_cast<float>(mx - session.editor.dragStartMx) * pxScale;
                    const float dy = static_cast<float>(my - session.editor.dragStartMy) * pxScale;
                    session.editor.cameraX = session.editor.dragStartCameraX - dx;
                    session.editor.cameraY = session.editor.dragStartCameraY + dy;
                }
            } else {
                session.editor.middleDragActive = false;
            }

            const float worldWidthPixels = static_cast<float>(session.editor.level.foliage.width) * editorPixelsPerTile;
            const float worldHeightPixels = static_cast<float>(session.editor.level.foliage.height) * editorPixelsPerTile;
            // Allow panning past 0 and max by a viewport-sized margin for easier edge editing.
            const float edgePadX = std::max(64.0F, static_cast<float>(framebufferWidth) * 0.35F);
            const float edgePadY = std::max(64.0F, static_cast<float>(framebufferHeight) * 0.35F);
            const float minCameraX = -edgePadX;
            const float minCameraY = -edgePadY;
            const float maxCameraX = std::max(0.0F, worldWidthPixels - static_cast<float>(framebufferWidth)) + edgePadX;
            const float maxCameraY = std::max(0.0F, worldHeightPixels - static_cast<float>(framebufferHeight)) + edgePadY;
            session.editor.cameraX = std::clamp(session.editor.cameraX, minCameraX, maxCameraX);
            session.editor.cameraY = std::clamp(session.editor.cameraY, minCameraY, maxCameraY);
            const float cameraX = session.editor.cameraX;
            const float cameraY = session.editor.cameraY;

            // Grid lines first (faint).
            glBindTexture(GL_TEXTURE_2D, 0);
            glColor4f(1.0F, 1.0F, 1.0F, 0.08F);
            glBegin(GL_LINES);
            for (std::uint32_t x = 0; x <= session.editor.level.foliage.width; ++x) {
                const float gx = static_cast<float>(x) * editorPixelsPerTile - cameraX;
                if (gx < 0.0F || gx > static_cast<float>(framebufferWidth)) continue;
                glVertex2f(gx, 0.0F - cameraY);
                glVertex2f(gx, worldHeightPixels - cameraY);
            }
            for (std::uint32_t y = 0; y <= session.editor.level.foliage.height; ++y) {
                const float gy = static_cast<float>(y) * editorPixelsPerTile - cameraY;
                if (gy < 0.0F || gy > static_cast<float>(framebufferHeight)) continue;
                glVertex2f(0.0F - cameraX, gy);
                glVertex2f(worldWidthPixels - cameraX, gy);
            }
            glEnd();

            // Tiles. Inactive layers are dimmed so the active layer stands out.
            auto drawEditorLayer = [&](const std::vector<opm::client::render::TileDrawEntry>& entries, float alpha) {
                for (const auto& entry : entries) {
                    const float x0 = entry.worldX * zoomScale - cameraX;
                    const float y0 = entry.worldY * zoomScale - cameraY;
                    const float x1 = x0 + entry.tileSize * zoomScale;
                    const float y1 = y0 + entry.tileSize * zoomScale;
                    if (x1 < 0.0F || x0 > static_cast<float>(framebufferWidth) ||
                        y1 < 0.0F || y0 > static_cast<float>(framebufferHeight)) {
                        continue;
                    }
                    static constexpr float kQuadUVs[4][2] = {
                        {0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}, {0.0F, 0.0F},
                    };
                    const int rot = entry.rotationSteps & 0x3;
                    const auto uv = [&](int corner) -> const float* {
                        return kQuadUVs[(corner + 4 - rot) % 4];
                    };
                    const auto it = textures.find(entry.tileAssetId);
                    if (it != textures.end()) {
                        glBindTexture(GL_TEXTURE_2D, it->second);
                        glColor4f(1.0F, 1.0F, 1.0F, alpha);
                        glBegin(GL_QUADS);
                        glTexCoord2f(uv(0)[0], uv(0)[1]); glVertex2f(x0, y0);
                        glTexCoord2f(uv(1)[0], uv(1)[1]); glVertex2f(x1, y0);
                        glTexCoord2f(uv(2)[0], uv(2)[1]); glVertex2f(x1, y1);
                        glTexCoord2f(uv(3)[0], uv(3)[1]); glVertex2f(x0, y1);
                        glEnd();
                    }
                }
            };
            const float activeAlpha = 1.0F;
            const float inactiveAlpha = 0.35F;
            const auto layerAlpha = [&](EditorLayer which) {
                return session.editor.activeLayer == which ? activeAlpha : inactiveAlpha;
            };
            drawEditorLayer(session.editor.entries.background, layerAlpha(EditorLayer::Background));
            // Spawn safe zone — dotted white outline, drawn between
            // background and foliage so foliage and actors render on top.
            {
                const auto z = opm::engine::computeSpawnSafeZone(session.editor.level);
                const float x0 = z.minX * editorPixelsPerTile - cameraX;
                const float y0 = z.minY * editorPixelsPerTile - cameraY;
                const float x1 = z.maxX * editorPixelsPerTile - cameraX;
                const float y1 = z.maxY * editorPixelsPerTile - cameraY;
                glBindTexture(GL_TEXTURE_2D, 0);
                glColor4f(1.0F, 1.0F, 1.0F, 0.85F);
                const float kDashPx = 6.0F * zoomScale;
                const float kGapPx  = 4.0F * zoomScale;
                const float step = kDashPx + kGapPx;
                glBegin(GL_LINES);
                for (float x = x0; x < x1; x += step) {
                    const float xe = std::min(x + kDashPx, x1);
                    glVertex2f(x, y0); glVertex2f(xe, y0);
                    glVertex2f(x, y1); glVertex2f(xe, y1);
                }
                for (float y = y0; y < y1; y += step) {
                    const float ye = std::min(y + kDashPx, y1);
                    glVertex2f(x0, y); glVertex2f(x0, ye);
                    glVertex2f(x1, y); glVertex2f(x1, ye);
                }
                glEnd();
            }
            drawEditorLayer(session.editor.entries.foliage,    layerAlpha(EditorLayer::Foliage));
            drawEditorLayer(session.editor.entries.foreground, layerAlpha(EditorLayer::Foreground));

            // Spawn marker (green) and Goal marker (gold).
            auto drawMarker = [&](float worldXTiles, float worldYTiles, float r, float g, float b) {
                const float markerSize = editorPixelsPerTile * 0.9F;
                const float cx = worldXTiles * editorPixelsPerTile - cameraX;
                const float cy = worldYTiles * editorPixelsPerTile - cameraY;
                glBindTexture(GL_TEXTURE_2D, 0);
                glColor4f(r, g, b, 0.55F);
                glBegin(GL_QUADS);
                glVertex2f(cx, cy);
                glVertex2f(cx + markerSize, cy);
                glVertex2f(cx + markerSize, cy + markerSize);
                glVertex2f(cx, cy + markerSize);
                glEnd();
            };
            drawMarker(session.editor.level.spawnX, session.editor.level.spawnY, 0.3F, 1.0F, 0.4F);
            drawMarker(session.editor.level.goalX, session.editor.level.goalY, 1.0F, 0.85F, 0.2F);

            // Actor placements: render the same enemy sprite the runtime
            // uses, with a colored tint per script type so the user can tell
            // them apart at a glance. Each frame uses its own pixel size
            // (zoomed to the editor's effective tile size), anchored at the
            // actor's foot tile so different-sized sprites pivot consistently.
            {
                const float editorScale = editorPixelsPerTile / kPixelsPerTile;
                const float bodyW = opm::engine::kPlayerWidthTiles * editorPixelsPerTile;
                const float fallbackW = kPlayerRenderWidthTiles  * editorPixelsPerTile;
                const float fallbackH = kPlayerRenderHeightTiles * editorPixelsPerTile;
                const float actorAlpha =
                    (session.editor.activeLayer == EditorLayer::Actors) ? 1.0F : 0.55F;
                for (const auto& s : session.editor.level.actors) {
                    const float bodyLeft   = s.x * editorPixelsPerTile - cameraX;
                    const float bodyBottom = s.y * editorPixelsPerTile - cameraY;
                    const EnemyRegistry& reg = (s.category == opm::engine::ActorCategory::Powerup)
                        ? powerupRegistry : enemyRegistry;
                    const EnemySprite* es = reg.spriteFor(s.enemyKind);
                    const Texture2D* tex = (es != nullptr && !es->frames.empty())
                        ? &es->frames[0] : nullptr;
                    float drawW = fallbackW;
                    float drawH = fallbackH;
                    if (tex != nullptr) {
                        const float texW = static_cast<float>(tex->width);
                        const float texH = static_cast<float>(tex->height);
                        if (es != nullptr && es->heightTiles > 0.0F && texH > 0.0F) {
                            drawH = es->heightTiles * editorPixelsPerTile;
                            drawW = drawH * (texW / texH);
                        } else {
                            drawW = texW * editorScale;
                            drawH = texH * editorScale;
                        }
                    }
                    const float ax0 = bodyLeft - (drawW - bodyW) * 0.5F;
                    const float ay0 = bodyBottom;
                    const float ax1 = ax0 + drawW;
                    const float ay1 = ay0 + drawH;
                    if (tex != nullptr) {
                        if (s.script == opm::engine::ActorScript::MoveToPlayer) {
                            glColor4f(1.0F, 0.7F, 0.7F, actorAlpha);
                        } else {
                            glColor4f(0.85F, 0.95F, 1.0F, actorAlpha);
                        }
                        drawTextureQuad(*tex, false, ax0, ay0, ax1, ay1);
                        glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
                    } else {
                        glBindTexture(GL_TEXTURE_2D, 0);
                        if (s.script == opm::engine::ActorScript::MoveToPlayer) {
                            glColor4f(1.0F, 0.4F, 0.4F, actorAlpha);
                        } else {
                            glColor4f(0.4F, 0.6F, 1.0F, actorAlpha);
                        }
                        glBegin(GL_QUADS);
                        glVertex2f(ax0, ay0);
                        glVertex2f(ax1, ay0);
                        glVertex2f(ax1, ay1);
                        glVertex2f(ax0, ay1);
                        glEnd();
                        glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
                    }
                }
            }

            // Cursor highlight + paint input.
#ifdef OPM_CLIENT_HAS_IMGUI
            const bool mouseCaptured = ImGui::GetIO().WantCaptureMouse;
#else
            const bool mouseCaptured = false;
#endif
            if (!mouseCaptured) {
                double mx = 0.0, my = 0.0;
                glfwGetCursorPos(window, &mx, &my);
                if (winW > 0 && winH > 0) {
                    // Map cursor (top-left origin in window) to framebuffer pixels (bottom-left origin in our ortho).
                    const float cursorPx = static_cast<float>(mx) * (static_cast<float>(framebufferWidth) / static_cast<float>(winW));
                    const float cursorPy = static_cast<float>(framebufferHeight) -
                        static_cast<float>(my) * (static_cast<float>(framebufferHeight) / static_cast<float>(winH));
                    const float worldPx = cursorPx + cameraX;
                    const float worldPy = cursorPy + cameraY;
                    const int tileX = static_cast<int>(worldPx / editorPixelsPerTile);
                    const int tileY = static_cast<int>(worldPy / editorPixelsPerTile);
                    auto& activeLayer = layerByEnum(session.editor.activeLayer);
                    const bool inBounds = tileX >= 0 && tileY >= 0 &&
                        tileX < static_cast<int>(activeLayer.width) && tileY < static_cast<int>(activeLayer.height);

                    if (inBounds) {
                        // Highlight outline.
                        const float hx0 = static_cast<float>(tileX) * editorPixelsPerTile - cameraX;
                        const float hy0 = static_cast<float>(tileY) * editorPixelsPerTile - cameraY;
                        glBindTexture(GL_TEXTURE_2D, 0);
                        glColor4f(1.0F, 1.0F, 1.0F, 0.6F);
                        glBegin(GL_LINE_LOOP);
                        glVertex2f(hx0, hy0);
                        glVertex2f(hx0 + editorPixelsPerTile, hy0);
                        glVertex2f(hx0 + editorPixelsPerTile, hy0 + editorPixelsPerTile);
                        glVertex2f(hx0, hy0 + editorPixelsPerTile);
                        glEnd();

                        const bool leftDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                        const bool rightDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                        const std::size_t flat = static_cast<std::size_t>(tileY) * activeLayer.width + static_cast<std::size_t>(tileX);

                        if (session.editor.activeLayer == EditorLayer::Actors) {
                            // Actor placement: tile-snapped. One actor per tile.
                            auto& actors = session.editor.level.actors;
                            const auto findAt = [&](int tx, int ty) -> std::size_t {
                                for (std::size_t i = 0; i < actors.size(); ++i) {
                                    if (static_cast<int>(actors[i].x) == tx
                                     && static_cast<int>(actors[i].y) == ty) {
                                        return i;
                                    }
                                }
                                return actors.size();
                            };
                            // Refuse to place inside the spawn safe zone.
                            // The dotted-outline overlay tells the user why
                            // the click did nothing.
                            const bool inSafeZone =
                                opm::engine::aabbOverlapsSpawnSafeZone(
                                    session.editor.level,
                                    static_cast<float>(tileX) + 0.05F,
                                    static_cast<float>(tileY) + 0.05F,
                                    0.9F, 0.9F);
                            if (leftDown && !inSafeZone) {
                                const std::size_t at = findAt(tileX, tileY);
                                opm::engine::ActorSpawn* slot = nullptr;
                                if (at < actors.size()) {
                                    slot = &actors[at];
                                } else {
                                    opm::engine::ActorSpawn s {};
                                    s.x = static_cast<float>(tileX);
                                    s.y = static_cast<float>(tileY);
                                    actors.push_back(s);
                                    slot = &actors.back();
                                }
                                slot->script = session.editor.selectedActorScript;
                                slot->diesWhenStomped  = session.editor.selectedActorDiesWhenStomped;
                                slot->canJumpObstacles = session.editor.selectedActorCanJumpObstacles;
                                slot->canJumpRandom    = session.editor.selectedActorCanJumpRandom;
                                slot->canFly           = session.editor.selectedActorCanFly;
                                slot->enemyKind        = session.editor.selectedActorKind;
                                slot->category         = session.editor.selectedActorCategory;
                                session.editor.dirty = true;
                            } else if (rightDown) {
                                const std::size_t at = findAt(tileX, tileY);
                                if (at < actors.size()) {
                                    actors.erase(actors.begin() + static_cast<std::ptrdiff_t>(at));
                                    session.editor.dirty = true;
                                }
                            }
                        } else if (leftDown) {
                            if (session.editor.placingSpawn) {
                                session.editor.level.spawnX = static_cast<float>(tileX);
                                session.editor.level.spawnY = static_cast<float>(tileY);
                                session.editor.placingSpawn = false;
                                session.editor.dirty = true;
                            } else if (session.editor.placingGoal) {
                                session.editor.level.goalX = static_cast<float>(tileX);
                                session.editor.level.goalY = static_cast<float>(tileY);
                                session.editor.placingGoal = false;
                                session.editor.dirty = true;
                            } else {
                                // Compare on base index so clicking a tile that
                                // already has the right id (just rotated) is a
                                // no-op, but a different tile overwrites with
                                // rotation reset to 0.
                                const auto existing =
                                    opm::engine::tileBaseIndex(activeLayer.tileIndices[flat]);
                                if (existing != session.editor.selectedTile) {
                                    activeLayer.tileIndices[flat] = session.editor.selectedTile;
                                    session.editor.dirty = true;
                                    rebuildActiveLayerEntries();
                                }
                            }
                        } else if (rightDown) {
                            if (activeLayer.tileIndices[flat] != 0U) {
                                activeLayer.tileIndices[flat] = 0U;
                                session.editor.dirty = true;
                                rebuildActiveLayerEntries();
                            }
                        }

                        // R-key rotates the tile under the cursor 90 deg CW.
                        // Only fires on key-press edges so holding R doesn't
                        // spin tiles every frame. Tile layers only — Actors
                        // layer ignores it.
                        const bool rNow = !kbCaptured
                            && glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
                        if (rNow && !session.editor.rotateKeyPrev
                            && session.editor.activeLayer != EditorLayer::Actors) {
                            const auto cell = activeLayer.tileIndices[flat];
                            const auto base = opm::engine::tileBaseIndex(cell);
                            if (base != 0U) {
                                const auto rot = static_cast<std::uint8_t>(
                                    (opm::engine::tileRotationSteps(cell) + 1U) & 0x3U);
                                activeLayer.tileIndices[flat] = opm::engine::makeTileCell(base, rot);
                                session.editor.dirty = true;
                                rebuildActiveLayerEntries();
                            }
                        }
                        session.editor.rotateKeyPrev = rNow;
                    }
                }
            }
        }
#endif

        // ---- Menu / picker / creator UI ----
#ifdef OPM_CLIENT_HAS_IMGUI
        if (session.state == AppState::MainMenu) {
            ScreenContext screenCtx { nullptr, renderCtx, assets, gNetwork.session.get() };
            mainMenu.renderUI(screenCtx);
        } else if (session.state == AppState::LevelPicker) {
            ScreenContext screenCtx { nullptr, renderCtx, assets, gNetwork.session.get() };
            levelPicker.renderUI(screenCtx);
        } else if (session.state == AppState::OnlineLevelSelect) {
            ScreenContext screenCtx { nullptr, renderCtx, assets, gNetwork.session.get() };
            onlineLevelSelect.tick(screenCtx, 0.0);
            onlineLevelSelect.renderUI(screenCtx);
        } else if (session.state == AppState::LevelCreator) {
            ScreenContext screenCtx { nullptr, renderCtx, assets, gNetwork.session.get(),
                framebufferWidth, framebufferHeight };
            levelCreator.renderUI(screenCtx);
        } else if (session.state == AppState::Playing) {
            ScreenContext screenCtx { nullptr, renderCtx, assets, gNetwork.session.get() };
            playing.renderUI(screenCtx);
        }

        ImGui::Render();
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
#endif

        // ---- Window title ----
        if (session.state == AppState::MainMenu) {
            glfwSetWindowTitle(window, "Open Platformer Maker");
        } else if (session.state == AppState::LevelPicker) {
            glfwSetWindowTitle(window, "Open Platformer Maker - Browse Levels");
        } else if (session.state == AppState::OnlineLevelSelect) {
            glfwSetWindowTitle(window, "Open Platformer Maker - Lobby");
        } else if (session.state == AppState::LevelCreator) {
            glfwSetWindowTitle(window, "Open Platformer Maker - Level Editor");
        } else if (session.isOnline && gNetwork.session) {
            const bool netConnected = gNetwork.session->isConnected();
            const std::uint32_t pingMs = gNetwork.session->getPingMs();
            std::string title = "Open Platformer Maker - Online";
            if (netConnected) {
                title += " [Ping: " + std::to_string(pingMs) + "ms]";
            } else {
                title += " [DISCONNECTED]";
            }
            glfwSetWindowTitle(window, title.c_str());
        } else {
            glfwSetWindowTitle(window, "Open Platformer Maker - Offline");
        }

        glfwSwapBuffers(window);
    }

    // RenderContext + AssetRegistry destructors release ImGui, GL textures,
    // window, and GLFW in the right order.
    return 0;
}
#else
int ClientApp::runWindow(const opm::assets::AssetManifest&,
                         const opm::engine::LevelData&,
                         const ClientArgs&)
{
    return 0;
}
#endif

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)

void ClientApp::rebuildEntriesFromLevel(LayeredEntries& out, const opm::engine::LevelData& level)
{
    out.background = opm::client::render::buildTileLayerDrawEntries(*manifest_, level.background, kPixelsPerTile);
    out.foliage    = opm::client::render::buildTileLayerDrawEntries(*manifest_, level.foliage,    kPixelsPerTile);
    out.foreground = opm::client::render::buildTileLayerDrawEntries(*manifest_, level.foreground, kPixelsPerTile);
}

void ClientApp::enterPlaying(bool online, const opm::engine::LevelData& levelData)
{
    session_.activeLevel = levelData;
    rebuildEntriesFromLevel(session_.entries, session_.activeLevel);
    session_.simulation.setLevel(levelData);
    session_.isOnline = online;
    session_.fromEditor = false; // caller sets true for Test Play
    session_.state = AppState::Playing;
    std::cout << "[client] entered playing mode online=" << (online ? "true" : "false")
              << " level=" << levelData.foliage.width << "x" << levelData.foliage.height << "\n";
}

std::string ClientApp::enterLevelPicker(const std::string& host, std::uint16_t port,
                                        GameSession::PickerIntent intent)
{
    const auto connectResult = tryConnect(gNetwork, host, port);
    if (!connectResult.ok) {
        return connectResult.message;
    }
    std::string status;
    if (!gNetwork.session->requestLevelList(2000U, session_.serverLevels, status)) {
        return "level list request failed: " + status;
    }
    session_.selectedLevelIndex = -1;
    session_.pickerStatus.clear();
    session_.pickerIntent = intent;
    session_.state = AppState::LevelPicker;
    return {};
}

void ClientApp::enterLevelCreator(const std::vector<opm::client::render::PaletteEntry>& palette)
{
    // The engine convention is: level y=0 is the bottom row (ground) and
    // y increases upward (sky). Default blank level: 60x16, ground top at
    // y=2 in the foliage layer with two fill rows below, spawn just above.
    opm::engine::LevelData blank;
    opm::engine::resizeAllLayers(blank, 60U, 16U);

    constexpr std::uint16_t kGroundTopMid = 2U;
    constexpr std::uint16_t kGroundFill = 5U;
    const std::uint32_t groundY = 2U;
    for (std::uint32_t x = 0; x < blank.foliage.width; ++x) {
        blank.foliage.tileIndices[static_cast<std::size_t>(groundY) * blank.foliage.width + x] = kGroundTopMid;
        for (std::uint32_t y = 0; y < groundY; ++y) {
            blank.foliage.tileIndices[static_cast<std::size_t>(y) * blank.foliage.width + x] = kGroundFill;
        }
    }
    blank.spawnX = 3.0F;
    blank.spawnY = static_cast<float>(groundY + 1U);
    blank.goalX = static_cast<float>(blank.foliage.width - 3U);
    blank.goalY = static_cast<float>(groundY + 1U);

    session_.editor = {};
    session_.editor.level = std::move(blank);
    session_.editor.resizeWidth = static_cast<int>(session_.editor.level.foliage.width);
    session_.editor.resizeHeight = static_cast<int>(session_.editor.level.foliage.height);
    rebuildEntriesFromLevel(session_.editor.entries, session_.editor.level);
    session_.editor.selectedTile = palette.empty() ? std::uint16_t {1} : palette.front().tileIndex;
    session_.editor.activeLayer = EditorLayer::Foliage;
    session_.state = AppState::LevelCreator;
}

void ClientApp::editLoadedLevel(opm::engine::LevelData loaded, const std::string& name,
                                const std::vector<opm::client::render::PaletteEntry>& palette)
{
    session_.editor = {};
    session_.editor.level = std::move(loaded);
    session_.editor.resizeWidth = static_cast<int>(session_.editor.level.foliage.width);
    session_.editor.resizeHeight = static_cast<int>(session_.editor.level.foliage.height);
    rebuildEntriesFromLevel(session_.editor.entries, session_.editor.level);
    session_.editor.selectedTile = palette.empty() ? std::uint16_t {1} : palette.front().tileIndex;
    session_.editor.activeLayer = EditorLayer::Foliage;
    // Pre-fill the name input so Save round-trips back to the same slot.
    const auto trimmed = name.substr(0, sizeof(session_.editor.nameInput) - 1U);
    std::snprintf(session_.editor.nameInput, sizeof(session_.editor.nameInput), "%s", trimmed.c_str());
    session_.state = AppState::LevelCreator;
}

void ClientApp::rebuildEditorEntries()
{
    rebuildEntriesFromLevel(session_.editor.entries, session_.editor.level);
}

opm::engine::TileLayer& ClientApp::layerByEnum(EditorLayer which)
{
    switch (which) {
        case EditorLayer::Background: return session_.editor.level.background;
        case EditorLayer::Foreground: return session_.editor.level.foreground;
        case EditorLayer::Foliage:    [[fallthrough]];
        default:                      return session_.editor.level.foliage;
    }
}

std::vector<opm::client::render::TileDrawEntry>& ClientApp::layerEntriesByEnum(EditorLayer which)
{
    switch (which) {
        case EditorLayer::Background: return session_.editor.entries.background;
        case EditorLayer::Foreground: return session_.editor.entries.foreground;
        case EditorLayer::Foliage:    [[fallthrough]];
        default:                      return session_.editor.entries.foliage;
    }
}

void ClientApp::rebuildActiveLayerEntries()
{
    auto& layer = layerByEnum(session_.editor.activeLayer);
    layerEntriesByEnum(session_.editor.activeLayer) =
        opm::client::render::buildTileLayerDrawEntries(*manifest_, layer, kPixelsPerTile);
}

#else

void ClientApp::rebuildEntriesFromLevel(LayeredEntries&, const opm::engine::LevelData&) {}
void ClientApp::enterPlaying(bool, const opm::engine::LevelData&) {}
std::string ClientApp::enterLevelPicker(const std::string&, std::uint16_t, GameSession::PickerIntent) { return {}; }
void ClientApp::enterLevelCreator(const std::vector<opm::client::render::PaletteEntry>&) {}
void ClientApp::editLoadedLevel(opm::engine::LevelData, const std::string&,
                                const std::vector<opm::client::render::PaletteEntry>&) {}
void ClientApp::rebuildEditorEntries() {}
opm::engine::TileLayer& ClientApp::layerByEnum(EditorLayer) {
    static opm::engine::TileLayer empty {};
    return empty;
}
std::vector<opm::client::render::TileDrawEntry>& ClientApp::layerEntriesByEnum(EditorLayer) {
    static std::vector<opm::client::render::TileDrawEntry> empty {};
    return empty;
}
void ClientApp::rebuildActiveLayerEntries() {}

#endif


int ClientApp::run()
{
    return run(ClientArgs{});
}

int ClientApp::run(const ClientArgs& args)
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    const auto manifest = loadAndPrintAssetSummary(OPM_CLIENT_RESOURCE_DIR);
    const auto fallbackLevel = opm::engine::createBasicLevel(220U, 16U);
    printTileLayerStubSummary(manifest, fallbackLevel);
    gNetwork.actors.resetLocalOnly();

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)
    return runWindow(manifest, fallbackLevel, args);
#else
    std::cout << "[client] No local graphics backend available at configure-time. Running stub mode.\n";
    return 0;
#endif
}


} // namespace opm::client
