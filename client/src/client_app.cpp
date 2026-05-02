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
#include "screens/lobby_browser_screen.hpp"
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

using opm::client::game::kInvalidServerIndex;
using opm::client::game::levelFromSnapshot;
using opm::client::game::NetworkSessionContext;
using opm::client::game::parseAddress;
using opm::client::game::fetchLobbyList;
using opm::client::game::joinNamedLobby;
using opm::client::game::tryConnect;

NetworkSessionContext gNetwork {};

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
using opm::client::render::AssetRegistry;
using opm::client::render::RenderContext;
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
    // Per-call alias only used by the editor's blank-level / loaded-level
    // entry methods that take the palette by reference.
    auto& palette = assets.palette;
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
    // Refresh the cached lobby list and transition into the LobbyBrowser
    // screen. Used by both MainMenu and the LobbyBrowser's Refresh button.
    auto refreshLobbiesAndShow = [&](const std::string& host, std::uint16_t port) -> std::string {
        std::vector<opm::client::game::LobbyListing> listings;
        const auto res = fetchLobbyList(gNetwork, host, port, listings);
        if (!res.ok) {
            return res.message;
        }
        session.availableLobbies.clear();
        session.availableLobbies.reserve(listings.size());
        for (const auto& l : listings) {
            session.availableLobbies.push_back(GameSession::LobbyEntry {
                .name = l.name, .players = l.players, .capacity = l.capacity});
        }
        session.selectedLobbyIndex = -1;
        session.lobbyBrowserStatus.clear();
        session.state = AppState::LobbyBrowser;
        return {};
    };

    MainMenuScreen mainMenu(session, MainMenuScreen::Callbacks {
        .onOpenLobbyBrowser = [&](const std::string& host, std::uint16_t port) -> std::string {
            return refreshLobbiesAndShow(host, port);
        },
        .onOpenLevelCreator = [&](const std::string& host, std::uint16_t port) -> std::string {
            // Connect (sessionless), fetch the level catalogue, then show
            // the picker which has both "Create New" and a list to load.
            return enterLevelPicker(host, port, GameSession::PickerIntent::EditOnServer);
        },
        .onQuit = [&]() {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        },
    });

    LobbyBrowserScreen lobbyBrowser(session, LobbyBrowserScreen::Callbacks {
        .onRefresh = [&](const std::string& host, std::uint16_t port) -> std::string {
            std::vector<opm::client::game::LobbyListing> listings;
            const auto res = fetchLobbyList(gNetwork, host, port, listings);
            if (!res.ok) {
                return res.message;
            }
            session.availableLobbies.clear();
            for (const auto& l : listings) {
                session.availableLobbies.push_back(GameSession::LobbyEntry {
                    .name = l.name, .players = l.players, .capacity = l.capacity});
            }
            return {};
        },
        .onJoin = [&](const std::string& host, std::uint16_t port,
                      const std::string& lobbyName) -> std::string {
            const auto res = joinNamedLobby(gNetwork, host, port, lobbyName);
            if (!res.ok) {
                return res.message;
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
    });

    LevelPickerScreen levelPicker(session, LevelPickerScreen::Callbacks {
        .onEditLoadedLevel = [&](opm::engine::LevelData loaded, const std::string& name) {
            editLoadedLevel(std::move(loaded), name);
        },
        .onCreateNewLevel = [&]() {
            enterLevelCreator();
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
                // Mirror the phase tail into GameSession so the lobby
                // UI can render the countdown / winner overlay.
                session.gamePhase = discard.phase;
                session.countdownTicks = discard.countdownTicks;
                session.winnerSlot = discard.winnerSlot;
            }
            // If the server transitioned us into Playing while we're
            // still on the lobby screen, auto-jump into the gameplay
            // screen using the latest snapshot the server pushed.
            if (session.gamePhase == opm::protocol::GamePhase::Playing
                && session.state == AppState::OnlineLevelSelect) {
                enterPlaying(true, gNetwork.networkLevel);
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
            // Pull MapVoteUpdate broadcasts so the lobby UI reflects
            // the latest tally without waiting for a poll-driven request.
            std::vector<std::vector<opm::protocol::MapVote>> voteUpdates;
            gNetwork.session->router().drainMapVoteUpdates(voteUpdates);
            if (!voteUpdates.empty()) {
                session.mapVoteTally = std::move(voteUpdates.back());
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
        .onCastVote = [&](const std::string& levelName) {
            if (!gNetwork.session) {
                return;
            }
            std::string status;
            (void)gNetwork.session->sendMapVote(levelName, status);
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

    PlayingScreen playing(session, PlayingScreen::Callbacks {
        .onLevelSnapshotChanged = [this](const opm::engine::LevelData& level) {
            rebuildEntriesFromLevel(session_.entries, level);
            session_.simulation.setLevel(level);
        },
    });

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
        .onRebuildActiveLayer = [this]() {
            rebuildActiveLayerEntries();
        },
    });

    // Gameplay timing now lives on PlayingScreen.

    // ---- Test-mode auto-join ----
    // When launched with --test-mode, skip the main menu entirely:
    // connect, join the first lobby, set the requested level, then start playing.
    if (clientArgs.testMode) {
        std::string host;
        std::uint16_t port = 0;
        if (parseAddress(clientArgs.testAddress, host, port)) {
            std::snprintf(session.addressInput, sizeof(session.addressInput), "%s", clientArgs.testAddress.c_str());
            // Test mode joins the first lobby automatically (no UI loop available).
            std::vector<opm::client::game::LobbyListing> listings;
            const auto listRes = fetchLobbyList(gNetwork, host, port, listings);
            opm::client::game::ConnectResult result {.ok = false, .message = listRes.message};
            if (listRes.ok && !listings.empty()) {
                result = joinNamedLobby(gNetwork, host, port, listings.front().name);
            } else if (listRes.ok) {
                result.message = "no lobbies advertised";
            }
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

        // ---- Tick gameplay ----
        // PlayingScreen owns the fixed-step loop, input gathering, and
        // network drain. ScreenContext provides the window handle +
        // NetworkSessionContext; the screen no-ops when state isn't Playing.
        {
            ScreenContext tickCtx { .app = nullptr, .render = renderCtx, .assets = assets,
                .session = gNetwork.session.get(), .net = &gNetwork,
                .window = window };
            (void)playing.tick(tickCtx, 0.0);
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
            ScreenContext screenCtx { .app = nullptr, .render = renderCtx, .assets = assets,
                .session = gNetwork.session.get(), .net = &gNetwork,
                .framebufferWidth = framebufferWidth, .framebufferHeight = framebufferHeight };
            playing.render(screenCtx);
        } else if (session.state == AppState::LevelCreator) {
            ScreenContext screenCtx { .app = nullptr, .render = renderCtx, .assets = assets,
                .session = gNetwork.session.get(),
                .window = window,
                .framebufferWidth = framebufferWidth, .framebufferHeight = framebufferHeight };
            levelCreator.render(screenCtx);
        }
#endif

        // ---- Menu / picker / creator UI ----
#ifdef OPM_CLIENT_HAS_IMGUI
        if (session.state == AppState::MainMenu) {
            ScreenContext screenCtx { .app = nullptr, .render = renderCtx, .assets = assets,
                .session = gNetwork.session.get() };
            mainMenu.renderUI(screenCtx);
        } else if (session.state == AppState::LobbyBrowser) {
            ScreenContext screenCtx { .app = nullptr, .render = renderCtx, .assets = assets,
                .session = gNetwork.session.get() };
            lobbyBrowser.renderUI(screenCtx);
        } else if (session.state == AppState::LevelPicker) {
            ScreenContext screenCtx { .app = nullptr, .render = renderCtx, .assets = assets,
                .session = gNetwork.session.get() };
            levelPicker.renderUI(screenCtx);
        } else if (session.state == AppState::OnlineLevelSelect) {
            ScreenContext screenCtx { .app = nullptr, .render = renderCtx, .assets = assets,
                .session = gNetwork.session.get(), .net = &gNetwork };
            onlineLevelSelect.tick(screenCtx, 0.0);
            onlineLevelSelect.renderUI(screenCtx);
        } else if (session.state == AppState::LevelCreator) {
            ScreenContext screenCtx { .app = nullptr, .render = renderCtx, .assets = assets,
                .session = gNetwork.session.get(),
                .framebufferWidth = framebufferWidth, .framebufferHeight = framebufferHeight };
            levelCreator.renderUI(screenCtx);
        } else if (session.state == AppState::Playing) {
            ScreenContext screenCtx { .app = nullptr, .render = renderCtx, .assets = assets,
                .session = gNetwork.session.get() };
            playing.renderUI(screenCtx);
        }

        ImGui::Render();
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
#endif

        // ---- Window title ----
        if (session.state == AppState::MainMenu) {
            glfwSetWindowTitle(window, "Open Platformer Maker");
        } else if (session.state == AppState::LobbyBrowser) {
            glfwSetWindowTitle(window, "Open Platformer Maker - Lobby Browser");
        } else if (session.state == AppState::LevelPicker) {
            glfwSetWindowTitle(window, "Open Platformer Maker - Level Studio");
        } else if (session.state == AppState::OnlineLevelSelect) {
            glfwSetWindowTitle(window, "Open Platformer Maker - Lobby");
        } else if (session.state == AppState::LevelCreator) {
            glfwSetWindowTitle(window, "Open Platformer Maker - Level Editor");
        } else if (gNetwork.session) {
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
            glfwSetWindowTitle(window, "Open Platformer Maker");
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
