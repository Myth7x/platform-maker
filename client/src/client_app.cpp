#include "client_app.hpp"

#include "net_client.hpp"
#include "render/sprite.hpp"
#include "render/texture.hpp"
#include "render/texture_loader.hpp"
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
#include <functional>
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
#include <charconv>
#include <string_view>
#endif

namespace opm::client {
namespace {

constexpr float kPixelsPerTile = 24.0F;
constexpr float kFixedStepSeconds = 1.0F / 60.0F;
constexpr float kPlayerRenderWidthTiles = 40.0F / kPixelsPerTile;
constexpr float kPlayerRenderHeightTiles = 40.0F / kPixelsPerTile;

constexpr std::uint16_t kInvalidServerIndex = 0xFFFFU;

struct Actor {
    bool isLocal {false};
    std::uint16_t serverIndex {kInvalidServerIndex};
    opm::protocol::PlayerInfo info {};
    opm::engine::PlayerState state {};
};

opm::engine::PlayerState remoteStateToPlayerState(const opm::client::net::RemotePlayerState& s)
{
    opm::engine::PlayerState p;
    p.position.x = s.positionX;
    p.position.y = s.positionY;
    p.velocity.x = s.velocityX;
    p.velocity.y = s.velocityY;
    p.onGround = s.onGround;
    p.facingRight = s.facingRight;
    p.skidding = s.skidding;
    p.crouching = s.crouching;
    p.pSpeedActive = s.pSpeedActive;
    p.pSpeedMeter = s.pSpeedMeter;
    p.style = static_cast<opm::engine::PlayerStyle>(s.style);
    p.powerupTransitionFrames = s.powerupTransitionFrames;
    p.invincibilityFrames = s.invincibilityFrames;
    return p;
}

class ActorManager {
public:
    void resetLocalOnly()
    {
        actors_.clear();
        Actor local;
        local.isLocal = true;
        actors_.push_back(local);
        lastServerTick_ = 0;
    }

    Actor* localActor()
    {
        if (actors_.empty()) {
            return nullptr;
        }
        return &actors_.front();
    }

    Actor* findByServerIndex(const std::uint16_t serverIndex)
    {
        for (auto& a : actors_) {
            if (a.serverIndex == serverIndex && serverIndex != kInvalidServerIndex) {
                return &a;
            }
        }
        return nullptr;
    }

    Actor& spawnRemote(const std::uint16_t serverIndex, const opm::protocol::PlayerInfo& info = {})
    {
        Actor remote;
        remote.isLocal = false;
        remote.serverIndex = serverIndex;
        remote.info = info;
        actors_.push_back(remote);
        return actors_.back();
    }

    void despawnRemote(const std::uint16_t serverIndex)
    {
        actors_.erase(
            std::remove_if(actors_.begin() + (actors_.empty() ? 0 : 1), actors_.end(),
                [serverIndex](const Actor& a) { return a.serverIndex == serverIndex; }),
            actors_.end());
    }

    void bindLocalToServer(const std::uint16_t serverIndex)
    {
        if (Actor* local = localActor()) {
            local->serverIndex = serverIndex;
        }
    }

    void applyRoster(const std::vector<opm::protocol::PlayerInfo>& roster, const std::uint16_t localServerIndex)
    {
        for (const auto& info : roster) {
            if (info.playerIndex == localServerIndex) {
                if (Actor* local = localActor()) {
                    local->info = info;
                }
                continue;
            }
            Actor* existing = findByServerIndex(info.playerIndex);
            if (info.connected && existing == nullptr) {
                spawnRemote(info.playerIndex, info);
            } else if (!info.connected && existing != nullptr) {
                despawnRemote(info.playerIndex);
            } else if (existing != nullptr) {
                existing->info = info;
            }
        }
    }

    void applyStateUpdate(const opm::client::net::StateUpdate& update, const std::uint16_t localServerIndex)
    {
        lastServerTick_ = update.serverTick;
        // Wire format is sparse — each record carries its own slotIndex.
        // Don't use the array position; the server only sends active slots.
        for (const auto& netPlayer : update.players) {
            const auto serverIndex = netPlayer.slotIndex;
            if (serverIndex == localServerIndex) {
                if (Actor* local = localActor()) {
                    local->state = remoteStateToPlayerState(netPlayer);
                }
                continue;
            }
            Actor* actor = findByServerIndex(serverIndex);
            if (actor == nullptr) {
                // Spawning is driven exclusively by roster updates so that the
                // server's default-spawn state for empty slots does not create
                // phantom remote actors.
                continue;
            }
            actor->state = remoteStateToPlayerState(netPlayer);
        }
        worldActors_.clear();
        worldActors_.reserve(update.actors.size());
        for (const auto& a : update.actors) {
            opm::engine::ActorState s {};
            s.position = {a.positionX, a.positionY};
            s.velocity = {a.velocityX, a.velocityY};
            s.alive = a.alive;
            s.facingRight = a.facingRight;
            s.script = static_cast<opm::engine::ActorScript>(a.script);
            s.enemyKind = a.enemyKind;
            s.category = static_cast<opm::engine::ActorCategory>(a.category);
            worldActors_.push_back(s);
        }
    }

    void setWorldActors(const std::vector<opm::engine::ActorState>& actors)
    {
        worldActors_ = actors;
    }
    [[nodiscard]] const std::vector<opm::engine::ActorState>& worldActors() const { return worldActors_; }

    void updateLocalState(const opm::engine::PlayerState& state)
    {
        if (Actor* local = localActor()) {
            local->state = state;
        }
    }

    [[nodiscard]] const std::vector<Actor>& actors() const { return actors_; }
    [[nodiscard]] std::size_t size() const { return actors_.size(); }
    [[nodiscard]] std::uint32_t lastServerTick() const { return lastServerTick_; }

private:
    std::vector<Actor> actors_;
    std::vector<opm::engine::ActorState> worldActors_ {};
    std::uint32_t lastServerTick_ {0};
};

struct NetworkSessionContext {
    std::unique_ptr<opm::client::net::SessionClient> session {};
    bool connected {false};
    std::uint16_t localPlayerIndex {kInvalidServerIndex};
    opm::engine::LevelData networkLevel {};
    ActorManager actors {};
};

NetworkSessionContext gNetwork {};

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
using opm::client::render::Texture2D;
using opm::client::render::AnimClip;
using opm::client::render::PlayerSprite;
using opm::client::render::PlayerSpriteSet;
using opm::client::render::EnemySprite;
using opm::client::render::EnemyRegistry;
using opm::client::render::PlayerFrameSelection;
using opm::client::render::loadPngRgba;
using opm::client::render::uploadTextureRgba;
using opm::client::render::loadTextureFromPath;
using opm::client::render::buildTileTextureMap;
using opm::client::render::destroyTileTextures;
using opm::client::render::drawTextureQuad;
using opm::client::render::loadPlayerSprite;
using opm::client::render::loadEnemySprite;
using opm::client::render::loadEnemyRegistry;
using opm::client::render::selectPlayerFrame;
using opm::client::render::selectEnemyFrame;
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

opm::engine::LevelData levelFromSnapshot(const opm::client::net::LevelSnapshot& snapshot)
{
    opm::engine::LevelData level;
    auto setLayer = [&](opm::engine::TileLayer& layer, const std::vector<std::uint16_t>& src) {
        layer.width = snapshot.width;
        layer.height = snapshot.height;
        const auto expected = static_cast<std::size_t>(snapshot.width) * static_cast<std::size_t>(snapshot.height);
        if (src.size() == expected) {
            layer.tileIndices = src;
        } else {
            // Tolerate older servers that didn't send a layer.
            layer.tileIndices.assign(expected, 0U);
        }
    };
    setLayer(level.background, snapshot.background);
    setLayer(level.foliage,    snapshot.foliage);
    setLayer(level.foreground, snapshot.foreground);
    level.spawnX = snapshot.spawnX;
    level.spawnY = snapshot.spawnY;
    level.goalX = snapshot.goalX;
    level.goalY = snapshot.goalY;
    return level;
}

struct ConnectResult {
    bool ok {false};
    std::string message;
};

// Parses "host:port" (e.g. "127.0.0.1:34900"). Returns false on malformed input.
bool parseAddress(std::string_view input, std::string& hostOut, std::uint16_t& portOut)
{
    const auto colon = input.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= input.size()) {
        return false;
    }
    hostOut.assign(input.substr(0, colon));
    const auto portStr = input.substr(colon + 1);
    int port = 0;
    const auto* begin = portStr.data();
    const auto* end = portStr.data() + portStr.size();
    auto [ptr, ec] = std::from_chars(begin, end, port);
    if (ec != std::errc {} || ptr != end || port <= 0 || port > 65535) {
        return false;
    }
    portOut = static_cast<std::uint16_t>(port);
    return true;
}

// Establishes a session-less connection (used for level browsing / saving).
ConnectResult tryConnect(const std::string& host, std::uint16_t port)
{
    if (!gNetwork.session) {
        gNetwork.session = std::make_unique<opm::client::net::SessionClient>();
    }
    std::string status;
    if (!gNetwork.session->connect(host, port, 1000U, status)) {
        return {false, "connect failed: " + status};
    }
    return {true, "connected"};
}

ConnectResult tryLobbyFlow(const std::string& host, std::uint16_t port)
{
    if (!gNetwork.session) {
        gNetwork.session = std::make_unique<opm::client::net::SessionClient>();
    }
    gNetwork.connected = false;
    gNetwork.actors.resetLocalOnly();

    std::string status;
    if (!gNetwork.session->connect(host, port, 1000U, status)) {
        std::cout << "[client] connect status: " << status << "\n";
        return {false, "connect failed: " + status};
    }

    std::vector<opm::client::net::LobbyInfo> lobbies;
    if (!gNetwork.session->requestLobbyList(1000U, lobbies, status)) {
        std::cout << "[client] lobby request status: " << status << "\n";
        return {false, "lobby list failed: " + status};
    }
    if (lobbies.empty()) {
        return {false, "no lobbies advertised by server"};
    }

    std::cout << "[client] received " << lobbies.size() << " lobby entries\n";
    for (const auto& lobby : lobbies) {
        std::cout << "[client] lobby=" << lobby.name << " players=" << lobby.players
                  << "/" << lobby.capacity << "\n";
    }

    opm::client::net::JoinResult joinResult;
    const bool joined = gNetwork.session->joinLobby(lobbies.front().name, 1000U, joinResult, status);
    std::cout << "[client] join lobby status: " << status
              << " success=" << (joined ? "true" : "false")
              << " player=" << static_cast<int>(joinResult.playerIndex)
              << " tickHz=" << joinResult.tickRateHz << "\n";

    if (!joined) {
        return {false, "join refused: " + status};
    }

    gNetwork.localPlayerIndex = joinResult.playerIndex;
    gNetwork.actors.bindLocalToServer(joinResult.playerIndex);
    gNetwork.actors.applyRoster(joinResult.roster, joinResult.playerIndex);

    opm::client::net::LevelSnapshot snapshot;
    if (gNetwork.session->receiveLevelSnapshot(1000U, snapshot, status)) {
        gNetwork.networkLevel = levelFromSnapshot(snapshot);
        opm::engine::PlayerState localSpawnState;
        localSpawnState.position.x = snapshot.spawnX;
        localSpawnState.position.y = snapshot.spawnY;
        gNetwork.actors.updateLocalState(localSpawnState);
        std::cout << "[client] level snapshot: " << snapshot.width << "x" << snapshot.height
                  << " tiles=" << snapshot.foliage.size() << " spawn=(" << snapshot.spawnX << "," << snapshot.spawnY
                  << ")\n";
    } else {
        return {false, "snapshot failed: " + status};
    }

    opm::client::net::StateUpdate update;
    if (gNetwork.session->pollStateUpdate(1000U, update, status)) {
        gNetwork.actors.applyStateUpdate(update, gNetwork.localPlayerIndex);
    }

    gNetwork.connected = true;
    std::cout << "[client] network session active host=" << host << " port=" << port << "\n";
    return {true, "connected"};
}

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
void drawDebugPlayerBody(const float x0, const float y0, const float x1, const float y1)
{
    glBindTexture(GL_TEXTURE_2D, 0);
    glColor4f(1.0F, 0.0F, 1.0F, 0.35F);
    glBegin(GL_QUADS);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();

    glColor3f(1.0F, 0.95F, 0.2F);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
}


void drawPSpeedHud(const float meterValue, const bool pSpeedActive, const int framebufferWidth, const int framebufferHeight)
{
    (void)framebufferWidth;
    const float clamped = std::clamp(meterValue, 0.0F, 1.0F);

    const float x = 12.0F;
    const float y = static_cast<float>(framebufferHeight) - 18.0F;
    const float w = 74.0F;
    const float h = 8.0F;

    glBindTexture(GL_TEXTURE_2D, 0);

    glColor4f(0.06F, 0.08F, 0.10F, 0.75F);
    glBegin(GL_QUADS);
    glVertex2f(x - 2.0F, y - 2.0F);
    glVertex2f(x + w + 2.0F, y - 2.0F);
    glVertex2f(x + w + 2.0F, y + h + 2.0F);
    glVertex2f(x - 2.0F, y + h + 2.0F);
    glEnd();

    const float fillW = w * clamped;
    if (fillW > 0.0F) {
        if (pSpeedActive) {
            glColor3f(1.0F, 0.86F, 0.16F);
        } else {
            glColor3f(0.80F, 0.22F, 0.22F);
        }
        glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + fillW, y);
        glVertex2f(x + fillW, y + h);
        glVertex2f(x, y + h);
        glEnd();
    }

    glColor3f(0.95F, 0.95F, 0.95F);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

#endif

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)

enum class AppState {
    MainMenu,
    LevelPicker,
    OnlineLevelSelect,
    LevelCreator,
    Playing,
};

struct PaletteEntry {
    std::uint16_t tileIndex {0};
    std::string assetId {};
    std::string subCategory {}; // e.g. "set1", "set2" — used for palette group headers
    GLuint texture {0};
};

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

struct GameSession {
    AppState state {AppState::MainMenu};
    bool isOnline {false};
    opm::engine::Simulation simulation;
    opm::engine::LevelData activeLevel;
    LayeredEntries entries;
    char addressInput[64] {"127.0.0.1:34900"};
    std::string menuStatus;

    // LevelPicker state.
    std::vector<std::string> serverLevels {};
    std::string pickerStatus {};
    int selectedLevelIndex {-1};
    // What the user is going to do with the picked level — controls the
    // confirm-button label and the action taken on click.
    enum class PickerIntent : std::uint8_t {
        PlayOffline = 0,
        EditOnServer = 1,
    };
    PickerIntent pickerIntent {PickerIntent::PlayOffline};

    // OnlineLevelSelect state (after a multiplayer lobby join, before play).
    std::vector<std::string> onlineLevels {};
    int onlineLevelSelected {-1};
    std::string onlineLevelStatus {};

    // LevelCreator state.
    LevelEditor editor {};

    // Set to true when Playing state was entered via "Test Play" from the editor.
    // Allows the "Back to Editor" button in the Playing HUD.
    bool fromEditor {false};
};

int runClientWindow(const opm::assets::AssetManifest& manifest, const opm::engine::LevelData& fallbackLevel,
                    const opm::client::ClientArgs& clientArgs)
{
    if (glfwInit() == GLFW_FALSE) {
        std::cerr << "[client] failed to initialize GLFW\n";
        return 1;
    }

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
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
#else
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#endif

    GLFWwindow* window = glfwCreateWindow(800, 600, "Open Platformer Maker", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "[client] failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);

    auto textures = buildTileTextureMap(manifest);
    std::cout << "[client] loaded tile textures: " << textures.size() << "\n";
    const std::filesystem::path actorsRoot =
        std::filesystem::path(OPM_CLIENT_RESOURCE_DIR) / "Actors";
    PlayerSpriteSet playerSpriteSet;
    playerSpriteSet.small = loadPlayerSprite(actorsRoot / "player" / "luigi" / "small");
    playerSpriteSet.big   = loadPlayerSprite(actorsRoot / "player" / "luigi" / "big");
    std::cout << "[client] player sprites — small: "
              << (playerSpriteSet.small.ready ? "ready" : "missing")
              << ", big: "
              << (playerSpriteSet.big.ready ? "ready" : "missing")
              << "\n";

    // Two parallel visual registries: enemies and powerups. Both use the
    // same sprite-directory scanner; the simulation routes actors to the
    // right registry via `ActorState::category`.
    auto enemyRegistry   = loadEnemyRegistry(actorsRoot / "enemies");
    auto powerupRegistry = loadEnemyRegistry(actorsRoot / "powerup");
    const auto logRegistry = [](const char* label, const EnemyRegistry& r) {
        if (r.size() > 0) {
            std::cout << "[client] " << label << " registry: " << r.size() << " kind(s):";
            for (std::size_t i = 0; i < r.size(); ++i) {
                std::cout << " [" << i << "]=" << r.names[i];
            }
            std::cout << "\n";
        } else {
            std::cout << "[client] " << label << " registry empty\n";
        }
    };
    logRegistry("enemy", enemyRegistry);
    logRegistry("powerup", powerupRegistry);

    auto colorForAsset = [](const std::string& assetId, float& r, float& g, float& b) {
        const auto h = static_cast<unsigned long long>(std::hash<std::string> {}(assetId));
        r = 0.2F + static_cast<float>((h >> 0U) & 0xFFU) / 510.0F;
        g = 0.2F + static_cast<float>((h >> 8U) & 0xFFU) / 510.0F;
        b = 0.2F + static_cast<float>((h >> 16U) & 0xFFU) / 510.0F;
    };
#endif

#ifdef OPM_CLIENT_HAS_IMGUI
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();
#endif

    // Build the tile palette: maps tile index (1..N) to its texture handle.
    std::vector<PaletteEntry> palette;
#ifdef OPM_CLIENT_WITH_OPENGL_STUB
    {
        for (const auto& record : manifest.records) {
            if (record.category != "Tiles") {
                continue;
            }
            constexpr std::string_view prefix = "Tiles:";
            if (!record.id.starts_with(prefix)) {
                continue;
            }
            std::uint16_t tileIndex = 0;
            const auto raw = record.id.substr(prefix.size());
            const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), tileIndex);
            if (result.ec != std::errc {}) {
                continue;
            }
            const auto it = textures.find(record.id);
            const GLuint handle = (it != textures.end()) ? it->second : 0U;
            palette.push_back(PaletteEntry {.tileIndex = tileIndex, .assetId = record.id, .subCategory = record.subCategory, .texture = handle});
        }
        std::sort(palette.begin(), palette.end(),
            [](const PaletteEntry& a, const PaletteEntry& b) { return a.tileIndex < b.tileIndex; });
        std::cout << "[client] palette entries: " << palette.size() << "\n";
    }
#endif

    GameSession session {};

    auto rebuildEntriesFromLevel = [&](LayeredEntries& out, const opm::engine::LevelData& level) {
        out.background = opm::client::render::buildTileLayerDrawEntries(manifest, level.background, kPixelsPerTile);
        out.foliage    = opm::client::render::buildTileLayerDrawEntries(manifest, level.foliage,    kPixelsPerTile);
        out.foreground = opm::client::render::buildTileLayerDrawEntries(manifest, level.foreground, kPixelsPerTile);
    };

    auto enterPlaying = [&](bool online, const opm::engine::LevelData& levelData) {
        session.activeLevel = levelData;
        rebuildEntriesFromLevel(session.entries, session.activeLevel);
        session.simulation.setLevel(levelData);
        session.isOnline = online;
        session.fromEditor = false; // caller sets true for Test Play
        session.state = AppState::Playing;
        std::cout << "[client] entered playing mode online=" << (online ? "true" : "false")
                  << " level=" << levelData.foliage.width << "x" << levelData.foliage.height << "\n";
    };

    auto enterLevelPicker = [&](const std::string& host, std::uint16_t port,
                                GameSession::PickerIntent intent) -> std::string {
        const auto connectResult = tryConnect(host, port);
        if (!connectResult.ok) {
            return connectResult.message;
        }
        std::string status;
        if (!gNetwork.session->requestLevelList(2000U, session.serverLevels, status)) {
            return "level list request failed: " + status;
        }
        session.selectedLevelIndex = -1;
        session.pickerStatus.clear();
        session.pickerIntent = intent;
        session.state = AppState::LevelPicker;
        return {};
    };

    auto enterLevelCreator = [&]() {
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

        session.editor = {};
        session.editor.level = std::move(blank);
        session.editor.resizeWidth = static_cast<int>(session.editor.level.foliage.width);
        session.editor.resizeHeight = static_cast<int>(session.editor.level.foliage.height);
        rebuildEntriesFromLevel(session.editor.entries, session.editor.level);
        session.editor.selectedTile = palette.empty() ? std::uint16_t {1} : palette.front().tileIndex;
        session.editor.activeLayer = EditorLayer::Foliage;
        session.state = AppState::LevelCreator;
    };

    // Variant for opening the editor on a pre-loaded server level. Same
    // setup as enterLevelCreator() but skips the blank-level scaffolding.
    auto editLoadedLevel = [&](opm::engine::LevelData loaded, const std::string& name) {
        session.editor = {};
        session.editor.level = std::move(loaded);
        session.editor.resizeWidth = static_cast<int>(session.editor.level.foliage.width);
        session.editor.resizeHeight = static_cast<int>(session.editor.level.foliage.height);
        rebuildEntriesFromLevel(session.editor.entries, session.editor.level);
        session.editor.selectedTile = palette.empty() ? std::uint16_t {1} : palette.front().tileIndex;
        session.editor.activeLayer = EditorLayer::Foliage;
        // Pre-fill the name input so Save round-trips back to the same slot.
        const auto trimmed = name.substr(0, sizeof(session.editor.nameInput) - 1U);
        std::snprintf(session.editor.nameInput, sizeof(session.editor.nameInput), "%s", trimmed.c_str());
        session.state = AppState::LevelCreator;
    };

    auto rebuildEditorEntries = [&]() {
        rebuildEntriesFromLevel(session.editor.entries, session.editor.level);
    };

    auto layerByEnum = [&](EditorLayer which) -> opm::engine::TileLayer& {
        switch (which) {
            case EditorLayer::Background: return session.editor.level.background;
            case EditorLayer::Foreground: return session.editor.level.foreground;
            case EditorLayer::Foliage:    [[fallthrough]];
            default:                      return session.editor.level.foliage;
        }
    };
    auto layerEntriesByEnum = [&](EditorLayer which) -> std::vector<opm::client::render::TileDrawEntry>& {
        switch (which) {
            case EditorLayer::Background: return session.editor.entries.background;
            case EditorLayer::Foreground: return session.editor.entries.foreground;
            case EditorLayer::Foliage:    [[fallthrough]];
            default:                      return session.editor.entries.foliage;
        }
    };
    auto rebuildActiveLayerEntries = [&]() {
        auto& layer = layerByEnum(session.editor.activeLayer);
        layerEntriesByEnum(session.editor.activeLayer) =
            opm::client::render::buildTileLayerDrawEntries(manifest, layer, kPixelsPerTile);
    };

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
            const auto result = tryLobbyFlow(host, port);
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
            opm::engine::PlayerState cameraPlayer {};
            if (!gNetwork.actors.actors().empty()) {
                cameraPlayer = gNetwork.actors.actors().front().state;
            }

            const float playerCenterPixelsX = (cameraPlayer.position.x + (opm::engine::kPlayerWidthTiles * 0.5F)) * kPixelsPerTile;
            const float playerCenterPixelsY = (cameraPlayer.position.y + (opm::engine::kPlayerHeightTiles * 0.5F)) * kPixelsPerTile;
            float cameraX = playerCenterPixelsX - static_cast<float>(framebufferWidth) * 0.35F;
            float cameraY = playerCenterPixelsY - static_cast<float>(framebufferHeight) * 0.35F;
            if (cameraX < 0.0F) {
                cameraX = 0.0F;
            }
            if (cameraY < 0.0F) {
                cameraY = 0.0F;
            }

            const float worldWidthPixels = static_cast<float>(session.activeLevel.foliage.width) * kPixelsPerTile;
            const float worldHeightPixels = static_cast<float>(session.activeLevel.foliage.height) * kPixelsPerTile;
            const float maxCameraX = std::max(0.0F, worldWidthPixels - static_cast<float>(framebufferWidth));
            const float maxCameraY = std::max(0.0F, worldHeightPixels - static_cast<float>(framebufferHeight));
            if (cameraX > maxCameraX) {
                cameraX = maxCameraX;
            }
            if (cameraY > maxCameraY) {
                cameraY = maxCameraY;
            }

            auto drawTileLayer = [&](const std::vector<opm::client::render::TileDrawEntry>& entries, float alpha) {
                for (const auto& entry : entries) {
                    const float x0 = entry.worldX - cameraX;
                    const float y0 = entry.worldY - cameraY;
                    const float x1 = x0 + entry.tileSize;
                    const float y1 = y0 + entry.tileSize;
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
                    } else {
                        float r = 0.5F, g = 0.8F, b = 0.3F;
                        colorForAsset(entry.tileAssetId, r, g, b);
                        glBindTexture(GL_TEXTURE_2D, 0);
                        glColor4f(r, g, b, alpha);
                        glBegin(GL_QUADS);
                        glVertex2f(x0, y0); glVertex2f(x1, y0);
                        glVertex2f(x1, y1); glVertex2f(x0, y1);
                        glEnd();
                    }
                }
            };
            // Draw order: background → safe-zone → foliage → players → foreground.
            // The dotted safe-zone outline sits between background and
            // foliage so foliage tiles, players, and actors all draw on top.
            drawTileLayer(session.entries.background, 1.0F);
            {
                const auto z = opm::engine::computeSpawnSafeZone(session.activeLevel);
                const float x0 = z.minX * kPixelsPerTile - cameraX;
                const float y0 = z.minY * kPixelsPerTile - cameraY;
                const float x1 = z.maxX * kPixelsPerTile - cameraX;
                const float y1 = z.maxY * kPixelsPerTile - cameraY;
                glBindTexture(GL_TEXTURE_2D, 0);
                glColor4f(1.0F, 1.0F, 1.0F, 0.85F);
                constexpr float kDashPx = 6.0F;
                constexpr float kGapPx = 4.0F;
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
            drawTileLayer(session.entries.foliage,    1.0F);

            const float bodyWidthPixels = opm::engine::kPlayerWidthTiles * kPixelsPerTile;
            // Fallback rectangle size used only when no texture is loaded.
            const float fallbackWidthPixels  = kPlayerRenderWidthTiles  * kPixelsPerTile;
            const float fallbackHeightPixels = kPlayerRenderHeightTiles * kPixelsPerTile;

            // Anchors a quad of pixel size (texW, texH) at the bottom-center
            // of the body (so feet sit on `bodyBottom` and the sprite stays
            // centered horizontally on `bodyLeft + bodyWidth/2`). Different
            // frame sizes within an animation just work — a tall idle and
            // a short crouch both pivot from the feet.
            const auto anchorBottomCenter = [](float bodyLeftWorld, float bodyBottomWorld,
                                               float bodyW, float texW, float texH,
                                               float& x0, float& y0, float& x1, float& y1) {
                x0 = bodyLeftWorld - (texW - bodyW) * 0.5F;
                y0 = bodyBottomWorld;
                x1 = x0 + texW;
                y1 = y0 + texH;
            };

            for (const auto& actor : gNetwork.actors.actors()) {
                const auto& player = actor.state;
                const float bodyLeftWorld   = (player.position.x * kPixelsPerTile) - cameraX;
                const float bodyBottomWorld = (player.position.y * kPixelsPerTile) - cameraY;

                // Two visual i-frame effects:
                //   * Power-up transition: draw the *opposite* style every
                //     few frames so the upgrade alternates Small <-> Big.
                //   * Post-damage i-frames: skip drawing the sprite every
                //     other ~3-frame slot so the player blinks. The
                //     underlying style is already Small at this point.
                opm::engine::PlayerStyle styleToDraw = player.style;
                if (player.powerupTransitionFrames > 0U) {
                    constexpr std::uint8_t kFlickerPeriodFrames = 4U;
                    const bool showOther =
                        ((player.powerupTransitionFrames / kFlickerPeriodFrames) & 1U) != 0U;
                    if (showOther) {
                        styleToDraw = (player.style == opm::engine::PlayerStyle::Big)
                            ? opm::engine::PlayerStyle::Small
                            : opm::engine::PlayerStyle::Big;
                    }
                }
                const bool blinkInvisible =
                    player.powerupTransitionFrames == 0U
                    && player.invincibilityFrames > 0U
                    && ((player.invincibilityFrames / 3U) & 1U) != 0U;
                if (blinkInvisible) {
                    continue; // skip drawing this frame entirely
                }
                const PlayerSprite& chosen = playerSpriteSet.forStyle(styleToDraw);
                const PlayerFrameSelection sel = selectPlayerFrame(chosen, player, animationTime);
                if (sel.tex != nullptr) {
                    // Per-clip height in tiles drives the on-screen size
                    // (e.g. crouch shrinks while idle stays tall); width
                    // derives from the frame's pixel aspect ratio so the
                    // sprite stays proportional. heightTiles == 0 falls
                    // back to the texture's native pixel size.
                    const float texW = static_cast<float>(sel.tex->width);
                    const float texH = static_cast<float>(sel.tex->height);
                    const float clipH = (sel.clip != nullptr) ? sel.clip->heightTiles : 0.0F;
                    float drawH = texH;
                    float drawW = texW;
                    if (clipH > 0.0F && texH > 0.0F) {
                        drawH = clipH * kPixelsPerTile;
                        drawW = drawH * (texW / texH);
                    }
                    float playerX0 = 0, playerY0 = 0, playerX1 = 0, playerY1 = 0;
                    anchorBottomCenter(
                        bodyLeftWorld, bodyBottomWorld, bodyWidthPixels,
                        drawW, drawH,
                        playerX0, playerY0, playerX1, playerY1);
                    drawTextureQuad(*sel.tex, false, playerX0, playerY0, playerX1, playerY1);
                } else {
                    float playerX0 = 0, playerY0 = 0, playerX1 = 0, playerY1 = 0;
                    anchorBottomCenter(
                        bodyLeftWorld, bodyBottomWorld, bodyWidthPixels,
                        fallbackWidthPixels, fallbackHeightPixels,
                        playerX0, playerY0, playerX1, playerY1);
                    drawDebugPlayerBody(playerX0, playerY0, playerX1, playerY1);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    glColor3f(0.95F, 0.2F, 0.2F);
                    glBegin(GL_QUADS);
                    glVertex2f(playerX0, playerY0);
                    glVertex2f(playerX1, playerY0);
                    glVertex2f(playerX1, playerY1);
                    glVertex2f(playerX0, playerY1);
                    glEnd();
                }
            }

            // Render world actors (enemies/NPCs) with the dedicated enemy
            // sprite. The frame index is driven by animation time so all
            // active actors stay in lockstep visually; flip the texture to
            // match facing direction when the source frames are right-only.
            for (const auto& a : gNetwork.actors.worldActors()) {
                if (!a.alive) {
                    continue;
                }
                const float bodyLeftWorld   = (a.position.x * kPixelsPerTile) - cameraX;
                const float bodyBottomWorld = (a.position.y * kPixelsPerTile) - cameraY;

                const EnemyRegistry& reg = (a.category == opm::engine::ActorCategory::Powerup)
                    ? powerupRegistry : enemyRegistry;
                const EnemySprite* es = reg.spriteFor(a.enemyKind);
                const Texture2D* tex = (es != nullptr)
                    ? selectEnemyFrame(*es, animationTime, a.velocity.x)
                    : nullptr;

                float aX0 = 0, aY0 = 0, aX1 = 0, aY1 = 0;
                if (tex != nullptr) {
                    const float texW = static_cast<float>(tex->width);
                    const float texH = static_cast<float>(tex->height);
                    float drawH = texH;
                    float drawW = texW;
                    if (es != nullptr && es->heightTiles > 0.0F && texH > 0.0F) {
                        drawH = es->heightTiles * kPixelsPerTile;
                        drawW = drawH * (texW / texH);
                    }
                    anchorBottomCenter(
                        bodyLeftWorld, bodyBottomWorld, bodyWidthPixels,
                        drawW, drawH,
                        aX0, aY0, aX1, aY1);
                } else {
                    anchorBottomCenter(
                        bodyLeftWorld, bodyBottomWorld, bodyWidthPixels,
                        fallbackWidthPixels, fallbackHeightPixels,
                        aX0, aY0, aX1, aY1);
                }
                if (tex != nullptr) {
                    drawTextureQuad(*tex, !a.facingRight, aX0, aY0, aX1, aY1);
                } else {
                    glBindTexture(GL_TEXTURE_2D, 0);
                    glColor3f(0.9F, 0.3F, 0.3F);
                    glBegin(GL_QUADS);
                    glVertex2f(aX0, aY0);
                    glVertex2f(aX1, aY0);
                    glVertex2f(aX1, aY1);
                    glVertex2f(aX0, aY1);
                    glEnd();
                }
            }

            // Foreground tiles render in front of the player.
            drawTileLayer(session.entries.foreground, 1.0F);

            const opm::engine::PlayerState hudPlayer = cameraPlayer;
            drawPSpeedHud(hudPlayer.pSpeedMeter, hudPlayer.pSpeedActive, framebufferWidth, framebufferHeight);
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
            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
                ImGuiCond_Always, ImVec2(0.5F, 0.5F));
            ImGui::SetNextWindowSize(ImVec2(460.0F, 0.0F));
            ImGui::Begin("Open Platformer Maker", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

            // Server address is hardcoded to the default; field hidden from menu.

            const auto resolveHostPort = [&](std::string& host, std::uint16_t& port) -> bool {
                if (!parseAddress(session.addressInput, host, port)) {
                    session.menuStatus = "Invalid address. Use host:port (e.g. 127.0.0.1:34900).";
                    return false;
                }
                return true;
            };

            if (ImGui::Button("Play Offline (browse server levels)", ImVec2(-1.0F, 36.0F))) {
                std::string host;
                std::uint16_t port = 0;
                if (resolveHostPort(host, port)) {
                    const auto err = enterLevelPicker(host, port,
                        GameSession::PickerIntent::PlayOffline);
                    session.menuStatus = err;
                }
            }

            if (ImGui::Button("Quick Offline (built-in level)", ImVec2(-1.0F, 28.0F))) {
                gNetwork.connected = false;
                gNetwork.localPlayerIndex = kInvalidServerIndex;
                gNetwork.actors.resetLocalOnly();
                enterPlaying(false, fallbackLevel);
                session.menuStatus.clear();
            }

            if (ImGui::Button("Play Online", ImVec2(-1.0F, 36.0F))) {
                std::string host;
                std::uint16_t port = 0;
                if (resolveHostPort(host, port)) {
                    const auto result = tryLobbyFlow(host, port);
                    if (result.ok) {
                        // Fetch the level catalogue right away so the user can
                        // pick which level the lobby should run.
                        std::string status;
                        session.onlineLevels.clear();
                        session.onlineLevelSelected = -1;
                        session.onlineLevelStatus.clear();
                        if (gNetwork.session) {
                            (void)gNetwork.session->requestLevelList(2000U, session.onlineLevels, status);
                        }
                        session.state = AppState::OnlineLevelSelect;
                        session.menuStatus.clear();
                    } else {
                        session.menuStatus = result.message;
                    }
                }
            }

            if (ImGui::Button("Create Level", ImVec2(-1.0F, 36.0F))) {
                enterLevelCreator();
                session.menuStatus.clear();
            }
            if (ImGui::Button("Edit Server Level", ImVec2(-1.0F, 36.0F))) {
                std::string host;
                std::uint16_t port = 0;
                if (resolveHostPort(host, port)) {
                    const auto err = enterLevelPicker(host, port,
                        GameSession::PickerIntent::EditOnServer);
                    session.menuStatus = err;
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("Quit", ImVec2(-1.0F, 28.0F))) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            if (!session.menuStatus.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.45F, 1.0F), "%s", session.menuStatus.c_str());
            }
            ImGui::End();
        } else if (session.state == AppState::LevelPicker) {
            const bool isEditIntent =
                session.pickerIntent == GameSession::PickerIntent::EditOnServer;
            const char* windowTitle = isEditIntent ? "Edit a Server Level" : "Choose a Level";
            const char* loadLabel   = isEditIntent ? "Load & Edit"          : "Load & Play Offline";

            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
                ImGuiCond_Always, ImVec2(0.5F, 0.5F));
            ImGui::SetNextWindowSize(ImVec2(460.0F, 360.0F));
            ImGui::Begin(windowTitle, nullptr,
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);

            ImGui::Text("Levels available on server (%zu):", session.serverLevels.size());
            ImGui::Separator();
            ImGui::BeginChild("levels", ImVec2(0.0F, 220.0F), true);
            for (int i = 0; i < static_cast<int>(session.serverLevels.size()); ++i) {
                const bool selected = session.selectedLevelIndex == i;
                if (ImGui::Selectable(session.serverLevels[i].c_str(), selected)) {
                    session.selectedLevelIndex = i;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    session.selectedLevelIndex = i;
                    // Trigger Load via the button below by simulating the click path:
                    // (handled below to avoid duplicating the load flow.)
                }
            }
            if (session.serverLevels.empty()) {
                ImGui::TextDisabled("(no levels — create one with the editor!)");
            }
            ImGui::EndChild();

            ImGui::Separator();
            const bool canLoad = session.selectedLevelIndex >= 0 &&
                session.selectedLevelIndex < static_cast<int>(session.serverLevels.size());
            ImGui::BeginDisabled(!canLoad);
            if (ImGui::Button(loadLabel, ImVec2(220.0F, 32.0F))) {
                const auto& name = session.serverLevels[static_cast<std::size_t>(session.selectedLevelIndex)];
                opm::engine::LevelData loaded;
                std::string status;
                if (gNetwork.session && gNetwork.session->requestLoadLevel(name, 2000U, loaded, status)) {
                    if (isEditIntent) {
                        // Open the editor on the loaded level. Stay
                        // connected so Save round-trips back to the server
                        // without a reconnect.
                        editLoadedLevel(std::move(loaded), name);
                    } else {
                        gNetwork.connected = false;
                        gNetwork.localPlayerIndex = kInvalidServerIndex;
                        gNetwork.actors.resetLocalOnly();
                        enterPlaying(false, loaded);
                    }
                    session.pickerStatus.clear();
                } else {
                    session.pickerStatus = "load failed: " + status;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Refresh", ImVec2(120.0F, 32.0F))) {
                std::string status;
                if (!gNetwork.session ||
                    !gNetwork.session->requestLevelList(2000U, session.serverLevels, status)) {
                    session.pickerStatus = "refresh failed: " + status;
                } else {
                    session.pickerStatus.clear();
                    session.selectedLevelIndex = -1;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Back", ImVec2(80.0F, 32.0F))) {
                session.state = AppState::MainMenu;
                session.pickerStatus.clear();
            }

            if (!session.pickerStatus.empty()) {
                ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.45F, 1.0F), "%s", session.pickerStatus.c_str());
            }
            ImGui::End();
        } else if (session.state == AppState::OnlineLevelSelect) {
            // Drain server traffic so the recv buffer doesn't fill while the
            // user picks. Snapshots arriving here update the lobby's "current"
            // level shown if they pick "Use current level".
            if (gNetwork.session && gNetwork.session->isConnected()) {
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
            }

            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
                ImGuiCond_Always, ImVec2(0.5F, 0.5F));
            ImGui::SetNextWindowSize(ImVec2(480.0F, 400.0F));
            ImGui::Begin("Lobby - Choose Level", nullptr,
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);

            ImGui::Text("Connected as player %d. Pick a level for this lobby:",
                static_cast<int>(gNetwork.localPlayerIndex));
            ImGui::Separator();

            ImGui::BeginChild("##online_levels", ImVec2(0.0F, 240.0F), true);
            for (int i = 0; i < static_cast<int>(session.onlineLevels.size()); ++i) {
                const bool selected = session.onlineLevelSelected == i;
                if (ImGui::Selectable(session.onlineLevels[i].c_str(), selected)) {
                    session.onlineLevelSelected = i;
                }
            }
            if (session.onlineLevels.empty()) {
                ImGui::TextDisabled("(no levels on server — use the editor first, or play with the default)");
            }
            ImGui::EndChild();

            ImGui::Separator();
            const bool canSet = session.onlineLevelSelected >= 0 &&
                session.onlineLevelSelected < static_cast<int>(session.onlineLevels.size()) &&
                gNetwork.session && gNetwork.session->isConnected();
            ImGui::BeginDisabled(!canSet);
            if (ImGui::Button("Use Selected Level", ImVec2(180.0F, 32.0F))) {
                const auto& name = session.onlineLevels[static_cast<std::size_t>(session.onlineLevelSelected)];
                std::string status;
                if (gNetwork.session->requestSetLobbyLevel(name, 2000U, status)) {
                    // The server broadcasts a fresh LevelSnapshot right after
                    // the response. Drain a few state updates briefly to give
                    // the snapshot time to arrive before we enter Playing.
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
                    session.onlineLevelStatus.clear();
                } else {
                    session.onlineLevelStatus = "set level failed: " + status;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Use Current Level", ImVec2(160.0F, 32.0F))) {
                enterPlaying(true, gNetwork.networkLevel);
                session.onlineLevelStatus.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh", ImVec2(90.0F, 32.0F))) {
                std::string status;
                if (!gNetwork.session ||
                    !gNetwork.session->requestLevelList(2000U, session.onlineLevels, status)) {
                    session.onlineLevelStatus = "refresh failed: " + status;
                } else {
                    session.onlineLevelStatus.clear();
                    session.onlineLevelSelected = -1;
                }
            }
            ImGui::Spacing();
            if (ImGui::Button("Disconnect", ImVec2(120.0F, 28.0F))) {
                if (gNetwork.session) {
                    gNetwork.session->disconnect();
                }
                gNetwork.connected = false;
                gNetwork.localPlayerIndex = kInvalidServerIndex;
                gNetwork.actors.resetLocalOnly();
                session.state = AppState::MainMenu;
                session.onlineLevelStatus.clear();
            }

            if (!session.onlineLevelStatus.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.45F, 1.0F), "%s", session.onlineLevelStatus.c_str());
            }
            ImGui::End();
        } else if (session.state == AppState::LevelCreator) {
            // ============================================================
            // Level editor layout — four anchored ImGui regions framing the
            // OpenGL viewport in the middle:
            //
            //   +-------------------------- top bar (file actions) -------+
            //   |                                                          |
            //   | left sidebar |       central viewport       | right     |
            //   | (layers /    |    (OpenGL grid + tiles      |  sidebar  |
            //   |  markers /   |     drawn behind ImGui)      |  context) |
            //   |  size)       |                              |           |
            //   |                                                          |
            //   +-------------------------- bottom status bar ------------+
            //
            // Each region is its own un-movable, un-resizable ImGui window
            // pinned to the framebuffer. ImGui's WantCaptureMouse already
            // suppresses paint clicks over the side panels, so the cursor /
            // paint code path elsewhere doesn't need to know about layout.
            // ============================================================
            constexpr float kTopBarH    = 38.0F;
            constexpr float kBottomBarH = 26.0F;
            constexpr float kLeftBarW   = 200.0F;
            constexpr float kRightBarW  = 260.0F;
            constexpr ImGuiWindowFlags kPanelFlags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize  | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings;
            const float fbW = static_cast<float>(framebufferWidth);
            const float fbH = static_cast<float>(framebufferHeight);

            // Reusable button helper for the toggle-style buttons used in
            // layer / type / script groups.
            const auto toggleButton = [&](const char* label, bool selected,
                                          const ImVec4& selectedColor,
                                          float widthPx, float heightPx) -> bool {
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, selectedColor);
                }
                const bool clicked = ImGui::Button(label, ImVec2(widthPx, heightPx));
                if (selected) {
                    ImGui::PopStyleColor();
                }
                return clicked;
            };

            // ===== Top bar: file / playtest actions =====
            ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
            ImGui::SetNextWindowSize(ImVec2(fbW, kTopBarH));
            ImGui::Begin("##creator_topbar", nullptr, kPanelFlags);
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Name");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(180.0F);
                ImGui::InputText("##levelname", session.editor.nameInput, sizeof(session.editor.nameInput));
                ImGui::SameLine();
                if (ImGui::Button("Save to server", ImVec2(140.0F, 0.0F))) {
                    std::string host;
                    std::uint16_t port = 0;
                    if (!parseAddress(session.addressInput, host, port)) {
                        session.editor.statusMessage = "Invalid server address.";
                    } else if (!gNetwork.session || !gNetwork.session->isConnected()) {
                        const auto cr = tryConnect(host, port);
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
                }
                ImGui::SameLine();
                if (ImGui::Button("Test Play", ImVec2(100.0F, 0.0F))) {
                    gNetwork.connected = false;
                    gNetwork.localPlayerIndex = kInvalidServerIndex;
                    gNetwork.actors.resetLocalOnly();
                    enterPlaying(false, session.editor.level);
                    session.fromEditor = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Back", ImVec2(70.0F, 0.0F))) {
                    session.state = AppState::MainMenu;
                }

                // Right-aligned dirty marker — pinned to the far edge so
                // it's easy to glance at without scanning the rest of the
                // bar.
                if (session.editor.dirty) {
                    const float markerW = 100.0F;
                    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - markerW);
                    ImGui::TextColored(ImVec4(1.0F, 0.85F, 0.4F, 1.0F),
                                       "* unsaved");
                }
            }
            ImGui::End();

            // ===== Left sidebar: layer + markers + size =====
            ImGui::SetNextWindowPos(ImVec2(0.0F, kTopBarH));
            ImGui::SetNextWindowSize(ImVec2(kLeftBarW, fbH - kTopBarH - kBottomBarH));
            ImGui::Begin("##creator_left", nullptr, kPanelFlags);
            {
                ImGui::TextDisabled("LAYERS");
                ImGui::Separator();
                const auto layerRow = [&](const char* label, EditorLayer layer) {
                    const bool selected = session.editor.activeLayer == layer;
                    const ImVec4 col(0.20F, 0.50F, 0.90F, 1.0F);
                    if (toggleButton(label, selected, col, -1.0F, 30.0F)) {
                        session.editor.activeLayer = layer;
                    }
                };
                layerRow("Background",        EditorLayer::Background);
                layerRow("Foliage (collide)", EditorLayer::Foliage);
                layerRow("Foreground",        EditorLayer::Foreground);
                layerRow("Actors",            EditorLayer::Actors);

                ImGui::Dummy(ImVec2(0.0F, 8.0F));
                ImGui::TextDisabled("MARKERS");
                ImGui::Separator();
                {
                    const ImVec4 spawnCol(0.30F, 0.80F, 0.40F, 1.0F);
                    if (toggleButton(session.editor.placingSpawn ? "Placing Spawn..." : "Set Spawn",
                                     session.editor.placingSpawn, spawnCol, -1.0F, 28.0F)) {
                        session.editor.placingSpawn = !session.editor.placingSpawn;
                        session.editor.placingGoal = false;
                    }
                    const ImVec4 goalCol(0.95F, 0.85F, 0.20F, 1.0F);
                    if (toggleButton(session.editor.placingGoal ? "Placing Goal..." : "Set Goal",
                                     session.editor.placingGoal, goalCol, -1.0F, 28.0F)) {
                        session.editor.placingGoal = !session.editor.placingGoal;
                        session.editor.placingSpawn = false;
                    }
                }

                ImGui::Dummy(ImVec2(0.0F, 8.0F));
                ImGui::TextDisabled("SIZE");
                ImGui::Separator();
                {
                    ImGui::TextUnformatted("Width");
                    ImGui::SetNextItemWidth(-1.0F);
                    ImGui::InputInt("##rw", &session.editor.resizeWidth, 1, 8);
                    ImGui::TextUnformatted("Height");
                    ImGui::SetNextItemWidth(-1.0F);
                    ImGui::InputInt("##rh", &session.editor.resizeHeight, 1, 8);
                    if (ImGui::Button("Apply", ImVec2(-1.0F, 26.0F))) {
                        const auto maxDim = static_cast<int>(opm::engine::kMaxLevelDimension);
                        int w = std::clamp(session.editor.resizeWidth, 1, maxDim);
                        int h = std::clamp(session.editor.resizeHeight, 1, maxDim);
                        session.editor.resizeWidth = w;
                        session.editor.resizeHeight = h;
                        opm::engine::resizeAllLayers(session.editor.level,
                            static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
                        rebuildEditorEntries();
                        session.editor.dirty = true;
                        session.editor.statusMessage = "Resized to " +
                            std::to_string(w) + "x" + std::to_string(h);
                    }
                    ImGui::TextDisabled("max %u", opm::engine::kMaxLevelDimension);
                }

                ImGui::Dummy(ImVec2(0.0F, 8.0F));
                ImGui::TextDisabled("HINTS");
                ImGui::Separator();
                ImGui::TextWrapped(
                    "Left-click: paint  -  Right-click: erase\n"
                    "Middle-drag / WASD: pan  -  Wheel: zoom\n"
                    "R: rotate hovered tile 90deg");
            }
            ImGui::End();

            // ===== Right sidebar: contextual content =====
            ImGui::SetNextWindowPos(ImVec2(fbW - kRightBarW, kTopBarH));
            ImGui::SetNextWindowSize(ImVec2(kRightBarW, fbH - kTopBarH - kBottomBarH));
            ImGui::Begin("##creator_right", nullptr, kPanelFlags);
            if (session.editor.activeLayer == EditorLayer::Actors) {
                // ---- Actor properties panel ----
                ImGui::TextDisabled("ACTOR");
                ImGui::Separator();

                ImGui::TextUnformatted("Type");
                {
                    const ImVec4 typeCol(0.85F, 0.55F, 0.20F, 1.0F);
                    const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
                    const auto pickType = [&](const char* label, opm::engine::ActorCategory cat, float w) {
                        const bool selected = session.editor.selectedActorCategory == cat;
                        if (toggleButton(label, selected, typeCol, w, 26.0F)) {
                            if (session.editor.selectedActorCategory != cat) {
                                session.editor.selectedActorCategory = cat;
                                // Avoid carrying an out-of-range index across
                                // registries with different sizes.
                                session.editor.selectedActorKind = 0;
                            }
                        }
                    };
                    pickType("Enemy",   opm::engine::ActorCategory::Enemy,   halfW);
                    ImGui::SameLine();
                    pickType("Powerup", opm::engine::ActorCategory::Powerup, halfW);
                }

                ImGui::Dummy(ImVec2(0.0F, 6.0F));
                ImGui::TextUnformatted("Sprite");
                {
                    const EnemyRegistry& activeReg =
                        (session.editor.selectedActorCategory == opm::engine::ActorCategory::Powerup)
                            ? powerupRegistry : enemyRegistry;
                    if (activeReg.names.empty()) {
                        ImGui::TextDisabled("(no sprites in registry)");
                    } else {
                        const int currentKind = std::min<int>(
                            static_cast<int>(session.editor.selectedActorKind),
                            static_cast<int>(activeReg.names.size()) - 1);
                        const char* preview = activeReg.names[currentKind].c_str();
                        ImGui::SetNextItemWidth(-1.0F);
                        if (ImGui::BeginCombo("##actorkind", preview)) {
                            for (int i = 0; i < static_cast<int>(activeReg.names.size()); ++i) {
                                const bool selected = currentKind == i;
                                if (ImGui::Selectable(activeReg.names[i].c_str(), selected)) {
                                    session.editor.selectedActorKind = static_cast<std::uint8_t>(i);
                                }
                                if (selected) {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                }

                ImGui::Dummy(ImVec2(0.0F, 6.0F));
                ImGui::TextUnformatted("Script");
                {
                    const ImVec4 scriptCol(0.20F, 0.70F, 0.40F, 1.0F);
                    const auto pickScript = [&](const char* label, opm::engine::ActorScript s) {
                        const bool selected = session.editor.selectedActorScript == s;
                        if (toggleButton(label, selected, scriptCol, -1.0F, 24.0F)) {
                            session.editor.selectedActorScript = s;
                        }
                    };
                    pickScript("Move random",    opm::engine::ActorScript::MoveRandom);
                    pickScript("Move to player", opm::engine::ActorScript::MoveToPlayer);
                }

                ImGui::Dummy(ImVec2(0.0F, 6.0F));
                ImGui::TextUnformatted("Behavior");
                ImGui::Checkbox("Dies on stomp",  &session.editor.selectedActorDiesWhenStomped);
                ImGui::Checkbox("Jumps obstacles", &session.editor.selectedActorCanJumpObstacles);
                ImGui::Checkbox("Jumps randomly",  &session.editor.selectedActorCanJumpRandom);
                ImGui::Checkbox("Can fly",         &session.editor.selectedActorCanFly);

                ImGui::Dummy(ImVec2(0.0F, 8.0F));
                ImGui::Separator();
                ImGui::TextWrapped(
                    "Click an empty tile: place a new actor with the\n"
                    "settings above. Click an existing actor: overwrite\n"
                    "its settings. Right-click: remove.");
            } else {
                // ---- Tile palette panel ----
                const char* layerLabel = "Foliage";
                ImVec4 layerColor(0.6F, 0.9F, 0.6F, 1.0F);
                if (session.editor.activeLayer == EditorLayer::Background) {
                    layerLabel = "Background";
                    layerColor = ImVec4(0.6F, 0.7F, 1.0F, 1.0F);
                } else if (session.editor.activeLayer == EditorLayer::Foreground) {
                    layerLabel = "Foreground";
                    layerColor = ImVec4(1.0F, 0.8F, 0.6F, 1.0F);
                }
                ImGui::TextDisabled("TILES");
                ImGui::SameLine();
                ImGui::TextColored(layerColor, "[%s]", layerLabel);
                ImGui::Separator();

                const bool eraserSelected = session.editor.selectedTile == 0U
                    && !session.editor.placingSpawn && !session.editor.placingGoal;
                ImGui::PushStyleColor(ImGuiCol_Button,
                    eraserSelected ? ImVec4(0.4F, 0.7F, 1.0F, 1.0F) : ImVec4(0.25F, 0.25F, 0.25F, 1.0F));
                if (ImGui::Button("Eraser", ImVec2(-1.0F, 30.0F))) {
                    session.editor.selectedTile = 0U;
                    session.editor.placingSpawn = false;
                    session.editor.placingGoal = false;
                }
                ImGui::PopStyleColor();

                ImGui::Spacing();
                constexpr float thumbSize = 40.0F;
                const float availWidth = ImGui::GetContentRegionAvail().x;
                const auto cols = std::max(1,
                    static_cast<int>(availWidth / (thumbSize + ImGui::GetStyle().ItemSpacing.x)));
                int columnCounter = 0;
                std::string currentPaletteSet;
                for (const auto& entry : palette) {
                    if (entry.subCategory != currentPaletteSet) {
                        if (columnCounter > 0) {
                            columnCounter = 0;
                        }
                        currentPaletteSet = entry.subCategory;
                        if (!currentPaletteSet.empty()) {
                            ImGui::Spacing();
                            ImGui::TextDisabled("%s", currentPaletteSet.c_str());
                            ImGui::Separator();
                        }
                    }
                    if (columnCounter > 0) {
                        ImGui::SameLine();
                    }
                    ImGui::PushID(static_cast<int>(entry.tileIndex));
                    const bool selected = session.editor.selectedTile == entry.tileIndex
                        && !session.editor.placingSpawn && !session.editor.placingGoal;
                    const ImVec4 tint(1.0F, 1.0F, 1.0F, 1.0F);
                    const ImVec4 bg = selected ? ImVec4(0.2F, 0.5F, 0.9F, 1.0F)
                                               : ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
                    if (entry.texture != 0U) {
                        // Plain (0,0)..(1,1) UVs: ImGui draws with a y-down
                        // ortho, so the V-flip used by the in-game renderer
                        // (which is y-up) would invert the thumbnail here.
                        if (ImGui::ImageButton("##t",
                                (ImTextureID)(intptr_t)entry.texture,
                                ImVec2(thumbSize, thumbSize),
                                ImVec2(0, 0), ImVec2(1, 1), bg, tint)) {
                            session.editor.selectedTile = entry.tileIndex;
                            session.editor.placingSpawn = false;
                            session.editor.placingGoal = false;
                        }
                    } else {
                        char label[16];
                        std::snprintf(label, sizeof(label), "%u", entry.tileIndex);
                        if (ImGui::Button(label, ImVec2(thumbSize, thumbSize))) {
                            session.editor.selectedTile = entry.tileIndex;
                            session.editor.placingSpawn = false;
                            session.editor.placingGoal = false;
                        }
                    }
                    ImGui::PopID();
                    if (++columnCounter >= cols) {
                        columnCounter = 0;
                    }
                }
            }
            ImGui::End();

            // ===== Collision inspector =====
            // A third right-side panel that pops in immediately to the
            // left of the palette whenever a tile is selected on a tile
            // layer. Edits a per-level override for the picked tile id —
            // every placement of that tile in the current level uses the
            // override, falling back to the default mask from
            // collisionMaskForTile when no entry exists.
            const bool tileLayerActive =
                session.editor.activeLayer == EditorLayer::Background
                || session.editor.activeLayer == EditorLayer::Foliage
                || session.editor.activeLayer == EditorLayer::Foreground;
            const bool inspectorOpen = tileLayerActive
                && session.editor.selectedTile != 0U
                && !session.editor.placingSpawn
                && !session.editor.placingGoal;
            if (inspectorOpen) {
                constexpr float kInspectorW = 240.0F;
                ImGui::SetNextWindowPos(
                    ImVec2(fbW - kRightBarW - kInspectorW, kTopBarH));
                ImGui::SetNextWindowSize(
                    ImVec2(kInspectorW, fbH - kTopBarH - kBottomBarH));
                ImGui::Begin("##creator_collision", nullptr, kPanelFlags);
                {
                    const std::uint16_t tileId = session.editor.selectedTile;
                    ImGui::TextDisabled("TILE %u", static_cast<unsigned>(tileId));
                    ImGui::Separator();

                    auto& overrides = session.editor.level.tileCollisionOverrides;
                    const auto it = overrides.find(tileId);
                    const bool hasOverride = (it != overrides.end());
                    opm::engine::TileCollisionMask mask = hasOverride
                        ? it->second
                        : opm::engine::collisionMaskForTile(tileId);

                    if (hasOverride) {
                        ImGui::TextColored(ImVec4(0.85F, 0.95F, 0.5F, 1.0F),
                                           "Custom override");
                    } else {
                        ImGui::TextDisabled("Default mask");
                    }
                    ImGui::Spacing();

                    ImGui::TextUnformatted("Solid faces");
                    bool changed = false;
                    if (ImGui::Checkbox("Top",    &mask.solidTop))    changed = true;
                    if (ImGui::Checkbox("Bottom", &mask.solidBottom)) changed = true;
                    if (ImGui::Checkbox("Left",   &mask.solidLeft))   changed = true;
                    if (ImGui::Checkbox("Right",  &mask.solidRight))  changed = true;

                    ImGui::Spacing();
                    ImGui::TextUnformatted("Special");
                    if (ImGui::Checkbox("Semi-solid (one-way top)", &mask.oneWayTop))
                        changed = true;

                    if (changed) {
                        overrides[tileId] = mask;
                        session.editor.dirty = true;
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::BeginDisabled(!hasOverride);
                    if (ImGui::Button("Reset to default", ImVec2(-1.0F, 28.0F))) {
                        overrides.erase(tileId);
                        session.editor.dirty = true;
                    }
                    ImGui::EndDisabled();

                    ImGui::Spacing();
                    ImGui::TextWrapped(
                        "Changes apply to every instance of this tile id "
                        "in this level. Reset clears the override.");
                }
                ImGui::End();
            }

            // ===== Bottom status bar =====
            ImGui::SetNextWindowPos(ImVec2(0.0F, fbH - kBottomBarH));
            ImGui::SetNextWindowSize(ImVec2(fbW, kBottomBarH));
            ImGui::Begin("##creator_status", nullptr, kPanelFlags);
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%ux%u",
                    session.editor.level.foliage.width,
                    session.editor.level.foliage.height);
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::TextDisabled("spawn (%.0f, %.0f)",
                    session.editor.level.spawnX, session.editor.level.spawnY);
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::TextDisabled("goal (%.0f, %.0f)",
                    session.editor.level.goalX, session.editor.level.goalY);
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::TextDisabled("zoom %.2fx", session.editor.zoom);

                if (session.editor.placingSpawn) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.7F, 0.95F, 0.5F, 1.0F),
                                       "Click to set SPAWN");
                } else if (session.editor.placingGoal) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.95F, 0.85F, 0.4F, 1.0F),
                                       "Click to set GOAL");
                }
                if (!session.editor.statusMessage.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6F, 0.95F, 0.5F, 1.0F),
                                       "%s", session.editor.statusMessage.c_str());
                }
            }
            ImGui::End();

            // ---- Canvas painting (handled below in input section) ----
        } else if (session.state == AppState::Playing && session.fromEditor) {
            // ---- Test Play HUD: Back to Editor button ----
            ImGui::SetNextWindowPos(ImVec2(8.0F, 8.0F), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(0.0F, 0.0F));
            ImGui::Begin("##testplay_hud", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::TextColored(ImVec4(1.0F, 0.85F, 0.3F, 1.0F), "TEST PLAY");
            ImGui::SameLine();
            if (ImGui::Button("Back to Editor")) {
                session.fromEditor = false;
                session.state = AppState::LevelCreator;
            }
            ImGui::End();
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

#ifdef OPM_CLIENT_HAS_IMGUI
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
#endif

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
    const auto deleteTextures = [](std::vector<Texture2D>& list) {
        for (auto& t : list) {
            if (t.handle != 0) {
                glDeleteTextures(1, &t.handle);
                t.handle = 0;
            }
        }
    };
    const auto deleteClip = [&](AnimClip& clip) {
        deleteTextures(clip.framesRight);
        deleteTextures(clip.framesLeft);
    };
    const auto deletePlayerSprite = [&](PlayerSprite& ps) {
        deleteClip(ps.idle);
        deleteClip(ps.crouch);
        deleteClip(ps.walkNormal);
        deleteClip(ps.walkNormalTurnaround);
        deleteClip(ps.airNormal);
        deleteClip(ps.walkPSpeed);
        deleteClip(ps.walkPSpeedTurnaround);
        deleteClip(ps.airPSpeed);
    };
    deletePlayerSprite(playerSpriteSet.small);
    deletePlayerSprite(playerSpriteSet.big);
    for (auto& es : enemyRegistry.sprites) {
        deleteTextures(es.frames);
    }
    for (auto& es : powerupRegistry.sprites) {
        deleteTextures(es.frames);
    }
    destroyTileTextures(textures);
#endif

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
#endif

} // namespace

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
    return runClientWindow(manifest, fallbackLevel, args);
#else
    std::cout << "[client] No local graphics backend available at configure-time. Running stub mode.\n";
    return 0;
#endif
}

} // namespace opm::client
