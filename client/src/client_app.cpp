#include "client_app.hpp"

#include "net_client.hpp"
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
#endif

#ifdef OPM_CLIENT_HAS_PNG
#include <png.h>
#endif

#ifdef OPM_CLIENT_HAS_STB_IMAGE
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#include <cstdio>
#include <vector>
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

constexpr std::uint8_t kInvalidServerIndex = 0xFFU;

struct Actor {
    bool isLocal {false};
    std::uint8_t serverIndex {kInvalidServerIndex};
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

    Actor* findByServerIndex(const std::uint8_t serverIndex)
    {
        for (auto& a : actors_) {
            if (a.serverIndex == serverIndex && serverIndex != kInvalidServerIndex) {
                return &a;
            }
        }
        return nullptr;
    }

    Actor& spawnRemote(const std::uint8_t serverIndex, const opm::protocol::PlayerInfo& info = {})
    {
        Actor remote;
        remote.isLocal = false;
        remote.serverIndex = serverIndex;
        remote.info = info;
        actors_.push_back(remote);
        return actors_.back();
    }

    void despawnRemote(const std::uint8_t serverIndex)
    {
        actors_.erase(
            std::remove_if(actors_.begin() + (actors_.empty() ? 0 : 1), actors_.end(),
                [serverIndex](const Actor& a) { return a.serverIndex == serverIndex; }),
            actors_.end());
    }

    void bindLocalToServer(const std::uint8_t serverIndex)
    {
        if (Actor* local = localActor()) {
            local->serverIndex = serverIndex;
        }
    }

    void applyRoster(const std::vector<opm::protocol::PlayerInfo>& roster, const std::uint8_t localServerIndex)
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

    void applyStateUpdate(const opm::client::net::StateUpdate& update, const std::uint8_t localServerIndex)
    {
        lastServerTick_ = update.serverTick;
        for (std::size_t i = 0; i < update.players.size(); ++i) {
            const auto serverIndex = static_cast<std::uint8_t>(i);
            if (serverIndex == localServerIndex) {
                if (Actor* local = localActor()) {
                    local->state = remoteStateToPlayerState(update.players[i]);
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
            actor->state = remoteStateToPlayerState(update.players[i]);
        }
    }

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
    std::uint32_t lastServerTick_ {0};
};

struct NetworkSessionContext {
    std::unique_ptr<opm::client::net::SessionClient> session {};
    bool connected {false};
    std::uint8_t localPlayerIndex {kInvalidServerIndex};
    opm::engine::LevelData networkLevel {};
    ActorManager actors {};
};

NetworkSessionContext gNetwork {};

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
struct Texture2D {
    GLuint handle {0};
    int width {0};
    int height {0};
};
#endif

struct SpriteFrame {
    int col {0};
    int row {0};
};

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
struct MarioSheet {
    Texture2D texture {};
    int frameWidth {40};
    int frameHeight {40};
    int frameOffsetX {0};
    int frameOffsetY {26};
    int cols {0};
    int rows {0};
    std::vector<int> occupancy {};
    std::vector<int> bottomPadding {};
    SpriteFrame fallbackFrame {4, 4};
};
#endif

struct SelectedSpriteFrame {
    SpriteFrame frame {};
    bool flipX {false};
};

SpriteFrame frameFromRowId(const int rowOneBased, const int idOneBased)
{
    // Spreadsheet ids/rows are 1-based: id 1 maps to x0, row 1 maps to y0.
    return SpriteFrame {std::max(0, idOneBased - 1), std::max(0, rowOneBased - 1)};
}

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
    const auto entries = opm::client::render::buildTileLayerDrawEntries(manifest, level.groundLayer, 24.0F);
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
    level.groundLayer.width = snapshot.width;
    level.groundLayer.height = snapshot.height;
    level.groundLayer.tileIndices = snapshot.tiles;
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
        std::cout << "[client] level snapshot: " << snapshot.width << "x" << snapshot.height
                  << " tiles=" << snapshot.tiles.size() << " spawn=(" << snapshot.spawnX << "," << snapshot.spawnY
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
bool loadPngRgba(const std::filesystem::path& path, std::vector<unsigned char>& pixels, int& width, int& height)
{
#ifdef OPM_CLIENT_HAS_STB_IMAGE
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    std::vector<unsigned char> fileBytes(static_cast<std::size_t>(size));
    if (std::fread(fileBytes.data(), 1, fileBytes.size(), file) != fileBytes.size()) {
        std::fclose(file);
        return false;
    }
    std::fclose(file);

    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(
        fileBytes.data(), static_cast<int>(fileBytes.size()), &width, &height, &channels, 4);
    if (decoded == nullptr) {
        return false;
    }
    pixels.assign(decoded, decoded + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    stbi_image_free(decoded);
    return true;
#elif defined(OPM_CLIENT_HAS_PNG)
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return false;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) {
        std::fclose(file);
        return false;
    }

    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(file);
        return false;
    }

    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(file);
        return false;
    }

    png_init_io(png, file);
    png_read_info(png, info);

    width = static_cast<int>(png_get_image_width(png, info));
    height = static_cast<int>(png_get_image_height(png, info));
    const png_byte colorType = png_get_color_type(png, info);
    const png_byte bitDepth = png_get_bit_depth(png, info);

    if (bitDepth == 16) {
        png_set_strip_16(png);
    }
    if (colorType == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS) != 0) {
        png_set_tRNS_to_alpha(png);
    }
    if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    png_read_update_info(png, info);

    pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    std::vector<png_bytep> rows(static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        rows[static_cast<std::size_t>(y)] = pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U;
    }

    png_read_image(png, rows.data());
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(file);
    return true;
#else
    (void)path;
    (void)pixels;
    (void)width;
    (void)height;
    return false;
#endif
}

Texture2D uploadTextureRgba(const std::vector<unsigned char>& rgba, const int width, const int height)
{
    if (rgba.empty() || width <= 0 || height <= 0) {
        return {};
    }

    Texture2D texture;
    glGenTextures(1, &texture.handle);
    glBindTexture(GL_TEXTURE_2D, texture.handle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        width,
        height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        rgba.data()
    );
    glBindTexture(GL_TEXTURE_2D, 0);

    texture.width = width;
    texture.height = height;
    return texture;
}

std::unordered_map<std::string, GLuint> buildTileTextureMap(const opm::assets::AssetManifest& manifest)
{
    std::unordered_map<std::string, GLuint> textures;

    for (const auto& record : manifest.records) {
        if (record.category != "Tiles") {
            continue;
        }

        std::vector<unsigned char> rgba;
        int width = 0;
        int height = 0;
        if (!loadPngRgba(record.path, rgba, width, height)) {
            continue;
        }

        const Texture2D texture = uploadTextureRgba(rgba, width, height);
        if (texture.handle != 0) {
            textures[record.id] = texture.handle;
        }
    }

    return textures;
}

MarioSheet loadMarioSheet(const std::filesystem::path& path)
{
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    if (!loadPngRgba(path, rgba, width, height)) {
        return {};
    }

    // If the sheet already has transparency, preserve it and avoid destructive color-keying.
    std::size_t transparentPixels = 0U;
    for (std::size_t i = 0; i + 3U < rgba.size(); i += 4U) {
        if (rgba[i + 3U] == 0U) {
            transparentPixels += 1U;
        }
    }

    if (transparentPixels == 0U) {
        const unsigned char keyR = rgba[0];
        const unsigned char keyG = rgba[1];
        const unsigned char keyB = rgba[2];
        for (std::size_t i = 0; i + 3U < rgba.size(); i += 4U) {
            const unsigned char r = rgba[i + 0U];
            const unsigned char g = rgba[i + 1U];
            const unsigned char b = rgba[i + 2U];
            const int dr = static_cast<int>(r) - static_cast<int>(keyR);
            const int dg = static_cast<int>(g) - static_cast<int>(keyG);
            const int db = static_cast<int>(b) - static_cast<int>(keyB);
            const int dist = (dr * dr) + (dg * dg) + (db * db);
            if (dist < 2400) {
                rgba[i + 3U] = 0U;
            }
        }
    }

    MarioSheet sheet;
    sheet.texture = uploadTextureRgba(rgba, width, height);
    sheet.cols = (width - sheet.frameOffsetX) / sheet.frameWidth;
    sheet.rows = (height - sheet.frameOffsetY) / sheet.frameHeight;
    if (sheet.cols <= 0 || sheet.rows <= 0) {
        return {};
    }
    sheet.occupancy.assign(static_cast<std::size_t>(sheet.cols * sheet.rows), 0);
    sheet.bottomPadding.assign(static_cast<std::size_t>(sheet.cols * sheet.rows), 0);

    int bestScore = -1;
    for (int row = 0; row < sheet.rows; ++row) {
        for (int col = 0; col < sheet.cols; ++col) {
            int usedPixels = 0;
            int maxOpaqueY = -1;
            for (int py = 0; py < sheet.frameHeight; ++py) {
                for (int px = 0; px < sheet.frameWidth; ++px) {
                    const int x = sheet.frameOffsetX + (col * sheet.frameWidth) + px;
                    const int y = sheet.frameOffsetY + (row * sheet.frameHeight) + py;
                    const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4U;
                    if (rgba[offset + 3U] > 16U) {
                        usedPixels += 1;
                        if (py > maxOpaqueY) {
                            maxOpaqueY = py;
                        }
                    }
                }
            }

            const int frameIndex = row * sheet.cols + col;
            sheet.occupancy[static_cast<std::size_t>(frameIndex)] = usedPixels;
            if (maxOpaqueY >= 0) {
                sheet.bottomPadding[static_cast<std::size_t>(frameIndex)] = (sheet.frameHeight - 1) - maxOpaqueY;
            }

            if (row >= 7 && row <= 12 && col >= 3 && col <= 21 && usedPixels > bestScore && usedPixels < 300) {
                bestScore = usedPixels;
                sheet.fallbackFrame = SpriteFrame {col, row};
            }
        }
    }

    if (bestScore < 0) {
        sheet.fallbackFrame = SpriteFrame {0, 0};
        bestScore = 0;
    }

    std::cout << "[client] luigi sheet: " << width << "x" << height
              << " frame=" << sheet.frameWidth << "x" << sheet.frameHeight
              << " offset=(" << sheet.frameOffsetX << "," << sheet.frameOffsetY << ")"
              << " grid=" << sheet.cols << "x" << sheet.rows
              << " using small-luigi bank\n";
    std::cout << "[client] luigi fallback frame: col=" << sheet.fallbackFrame.col
              << " row=" << sheet.fallbackFrame.row << " score=" << bestScore << "\n";

    return sheet;
}

SelectedSpriteFrame selectMarioFrame(const MarioSheet& sheet, const opm::engine::PlayerState& player, const float animTime)
{
    (void)sheet;
    // Explicit user-provided Luigi frame mapping (1-based row/id).
    // This sheet's prepared extraction aligns these states on row 4.
    const bool facingRight = player.facingRight;
    constexpr int kMovementRow = 4; // 1-based row from spreadsheet
    constexpr int kPSpeedRow = kMovementRow + 2;

    if (player.crouching) {
        return {facingRight ? frameFromRowId(kMovementRow, 10) : frameFromRowId(kMovementRow, 1), false};
    }

    if (!player.onGround) {
        return {facingRight ? frameFromRowId(kMovementRow, 9) : frameFromRowId(kMovementRow, 2), false};
    }

    const float speed = std::fabs(player.velocity.x);
    if (speed < 0.1F) {
        return {facingRight ? frameFromRowId(kMovementRow, 6) : frameFromRowId(kMovementRow, 5), false};
    }

    const int frame = static_cast<int>(animTime * (speed > 8.5F ? 14.0F : 10.0F)) % 3;
    if (player.pSpeedActive) {
        if (facingRight) {
            constexpr int kRightPSpeedIds[3] = {6, 7, 8};
            return {frameFromRowId(kPSpeedRow, kRightPSpeedIds[frame]), false};
        }
        constexpr int kLeftPSpeedIds[3] = {5, 4, 3};
        return {frameFromRowId(kPSpeedRow, kLeftPSpeedIds[frame]), false};
    }

    if (facingRight) {
        constexpr int kRightIds[3] = {6, 7, 8};
        return {frameFromRowId(kMovementRow, kRightIds[frame]), false};
    }
    constexpr int kLeftIds[3] = {5, 4, 3};
    return {frameFromRowId(kMovementRow, kLeftIds[frame]), false};
}

SpriteFrame makeVisibleFrame(const MarioSheet& sheet, SpriteFrame requested)
{
    if (requested.col < 0 || requested.row < 0 || requested.col >= sheet.cols || requested.row >= sheet.rows) {
        return sheet.fallbackFrame;
    }

    const auto idx = static_cast<std::size_t>(requested.row * sheet.cols + requested.col);
    if (idx >= sheet.occupancy.size()) {
        return sheet.fallbackFrame;
    }

    if (sheet.occupancy[idx] < 20) {
        return sheet.fallbackFrame;
    }
    return requested;
}

float frameGroundOffsetPixels(const MarioSheet& sheet, const SpriteFrame frame, const float renderHeightPixels)
{
    if (frame.col < 0 || frame.row < 0 || frame.col >= sheet.cols || frame.row >= sheet.rows) {
        return 0.0F;
    }

    const auto idx = static_cast<std::size_t>(frame.row * sheet.cols + frame.col);
    if (idx >= sheet.bottomPadding.size()) {
        return 0.0F;
    }

    const float scaleY = renderHeightPixels / static_cast<float>(sheet.frameHeight);
    return static_cast<float>(sheet.bottomPadding[idx]) * scaleY;
}

void drawSpriteFrame(
    const MarioSheet& sheet,
    const SpriteFrame frame,
    const bool flipX,
    const float x0,
    const float y0,
    const float x1,
    const float y1
)
{
    if (sheet.texture.handle == 0 || sheet.texture.width <= 0 || sheet.texture.height <= 0) {
        return;
    }
    if (frame.col < 0 || frame.row < 0 || frame.col >= sheet.cols || frame.row >= sheet.rows) {
        return;
    }

    const int texX0 = sheet.frameOffsetX + (frame.col * sheet.frameWidth);
    const int texX1 = texX0 + sheet.frameWidth;
    const int texY0 = sheet.frameOffsetY + (frame.row * sheet.frameHeight);
    const int texY1 = texY0 + sheet.frameHeight;

    const float u0 = static_cast<float>(texX0) / static_cast<float>(sheet.texture.width);
    const float u1 = static_cast<float>(texX1) / static_cast<float>(sheet.texture.width);
    const float vTop = static_cast<float>(texY0) / static_cast<float>(sheet.texture.height);
    const float vBottom = static_cast<float>(texY1) / static_cast<float>(sheet.texture.height);

    const float leftU = flipX ? u1 : u0;
    const float rightU = flipX ? u0 : u1;

    glBindTexture(GL_TEXTURE_2D, sheet.texture.handle);
    glColor3f(1.0F, 1.0F, 1.0F);
    glBegin(GL_QUADS);
    glTexCoord2f(leftU, vBottom);
    glVertex2f(x0, y0);
    glTexCoord2f(rightU, vBottom);
    glVertex2f(x1, y0);
    glTexCoord2f(rightU, vTop);
    glVertex2f(x1, y1);
    glTexCoord2f(leftU, vTop);
    glVertex2f(x0, y1);
    glEnd();
}

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

void destroyTileTextures(std::unordered_map<std::string, GLuint>& textures)
{
    for (auto& [id, texture] : textures) {
        (void)id;
        if (texture != 0U) {
            glDeleteTextures(1, &texture);
        }
    }
    textures.clear();
}
#endif

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)

enum class AppState {
    MainMenu,
    Playing,
};

struct GameSession {
    AppState state {AppState::MainMenu};
    bool isOnline {false};
    opm::engine::Simulation simulation;
    opm::engine::LevelData activeLevel;
    std::vector<opm::client::render::TileDrawEntry> entries;
    char addressInput[64] {"127.0.0.1:34900"};
    std::string menuStatus;
};

int runClientWindow(const opm::assets::AssetManifest& manifest, const opm::engine::LevelData& fallbackLevel)
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
    auto mario = loadMarioSheet(std::filesystem::path(OPM_CLIENT_RESOURCE_DIR) / "Sprites" / "luigi.png");
    if (mario.texture.handle != 0) {
        std::cout << "[client] luigi sprite ready\n";
    } else {
        std::cout << "[client] luigi sprite missing, fallback rectangle enabled\n";
    }

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

    GameSession session {};

    auto enterPlaying = [&](bool online, const opm::engine::LevelData& levelData) {
        session.activeLevel = levelData;
        session.entries = opm::client::render::buildTileLayerDrawEntries(manifest, levelData.groundLayer, kPixelsPerTile);
        session.simulation.setLevel(levelData);
        session.isOnline = online;
        session.state = AppState::Playing;
        std::cout << "[client] entered playing mode online=" << (online ? "true" : "false")
                  << " level=" << levelData.groundLayer.width << "x" << levelData.groundLayer.height << "\n";
    };

    bool jumpHeldLast = false;
    double previousTime = glfwGetTime();
    double accumulator = 0.0;
    float animationTime = 0.0F;

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

            const float playerCenterPixels = (cameraPlayer.position.x + (opm::engine::kPlayerWidthTiles * 0.5F)) * kPixelsPerTile;
            float cameraX = playerCenterPixels - static_cast<float>(framebufferWidth) * 0.35F;
            if (cameraX < 0.0F) {
                cameraX = 0.0F;
            }

            const float worldWidthPixels = static_cast<float>(session.activeLevel.groundLayer.width) * kPixelsPerTile;
            const float maxCameraX = std::max(0.0F, worldWidthPixels - static_cast<float>(framebufferWidth));
            if (cameraX > maxCameraX) {
                cameraX = maxCameraX;
            }

            for (const auto& entry : session.entries) {
                const float x0 = entry.worldX - cameraX;
                const float y0 = entry.worldY;
                const float x1 = x0 + entry.tileSize;
                const float y1 = y0 + entry.tileSize;

                if (x1 < 0.0F || x0 > static_cast<float>(framebufferWidth)) {
                    continue;
                }

                const auto it = textures.find(entry.tileAssetId);
                if (it != textures.end()) {
                    glBindTexture(GL_TEXTURE_2D, it->second);
                    glColor3f(1.0F, 1.0F, 1.0F);

                    glBegin(GL_QUADS);
                    glTexCoord2f(0.0F, 1.0F);
                    glVertex2f(x0, y0);
                    glTexCoord2f(1.0F, 1.0F);
                    glVertex2f(x1, y0);
                    glTexCoord2f(1.0F, 0.0F);
                    glVertex2f(x1, y1);
                    glTexCoord2f(0.0F, 0.0F);
                    glVertex2f(x0, y1);
                    glEnd();
                } else {
                    float r = 0.5F;
                    float g = 0.8F;
                    float b = 0.3F;
                    colorForAsset(entry.tileAssetId, r, g, b);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    glColor3f(r, g, b);

                    glBegin(GL_QUADS);
                    glVertex2f(x0, y0);
                    glVertex2f(x1, y0);
                    glVertex2f(x1, y1);
                    glVertex2f(x0, y1);
                    glEnd();
                }
            }

            const float bodyWidthPixels = opm::engine::kPlayerWidthTiles * kPixelsPerTile;
            const float renderWidthPixels = kPlayerRenderWidthTiles * kPixelsPerTile;
            const float renderHeightPixels = kPlayerRenderHeightTiles * kPixelsPerTile;

            for (const auto& actor : gNetwork.actors.actors()) {
                const auto& player = actor.state;
                const float playerX0 = (player.position.x * kPixelsPerTile) - cameraX - (renderWidthPixels - bodyWidthPixels) * 0.5F;
                const float playerY0 = player.position.y * kPixelsPerTile;
                const float playerX1 = playerX0 + renderWidthPixels;
                const float playerY1 = playerY0 + renderHeightPixels;

                if (mario.texture.handle != 0) {
                    const SelectedSpriteFrame selected = selectMarioFrame(mario, player, animationTime);
                    const SpriteFrame frame = makeVisibleFrame(mario, selected.frame);
                    const float groundOffset = frameGroundOffsetPixels(mario, frame, renderHeightPixels);
                    drawSpriteFrame(mario, frame, selected.flipX, playerX0, playerY0 - groundOffset, playerX1, playerY1 - groundOffset);
                } else {
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

            const opm::engine::PlayerState hudPlayer = cameraPlayer;
            drawPSpeedHud(hudPlayer.pSpeedMeter, hudPlayer.pSpeedActive, framebufferWidth, framebufferHeight);
        }
#endif

        // ---- Build menu UI (state == MainMenu) ----
#ifdef OPM_CLIENT_HAS_IMGUI
        if (session.state == AppState::MainMenu) {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
                ImGuiCond_Always, ImVec2(0.5F, 0.5F));
            ImGui::SetNextWindowSize(ImVec2(440.0F, 0.0F));
            ImGui::Begin("Open Platformer Maker", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

            ImGui::TextUnformatted("Choose a mode to start.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Play Offline", ImVec2(-1.0F, 36.0F))) {
                gNetwork.connected = false;
                gNetwork.localPlayerIndex = kInvalidServerIndex;
                gNetwork.actors.resetLocalOnly();
                enterPlaying(false, fallbackLevel);
                session.menuStatus.clear();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted("Online server (host:port)");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputText("##address", session.addressInput, sizeof(session.addressInput));

            if (ImGui::Button("Play Online", ImVec2(-1.0F, 36.0F))) {
                std::string host;
                std::uint16_t port = 0;
                if (!parseAddress(session.addressInput, host, port)) {
                    session.menuStatus = "Invalid address. Use host:port (e.g. 127.0.0.1:34900).";
                } else {
                    const auto result = tryLobbyFlow(host, port);
                    if (result.ok) {
                        enterPlaying(true, gNetwork.networkLevel);
                        session.menuStatus.clear();
                    } else {
                        session.menuStatus = result.message;
                    }
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
        }

        ImGui::Render();
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
#endif

        // ---- Window title ----
        if (session.state == AppState::MainMenu) {
            glfwSetWindowTitle(window, "Open Platformer Maker");
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
    if (mario.texture.handle != 0) {
        glDeleteTextures(1, &mario.texture.handle);
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
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    const auto manifest = loadAndPrintAssetSummary(OPM_CLIENT_RESOURCE_DIR);
    const auto fallbackLevel = opm::engine::createBasicLevel(220U, 16U);
    printTileLayerStubSummary(manifest, fallbackLevel);
    gNetwork.actors.resetLocalOnly();

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)
    return runClientWindow(manifest, fallbackLevel);
#else
    std::cout << "[client] No local graphics backend available at configure-time. Running stub mode.\n";
    return 0;
#endif
}

} // namespace opm::client
