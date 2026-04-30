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

namespace opm::client {
namespace {

constexpr float kPixelsPerTile = 24.0F;
constexpr float kFixedStepSeconds = 1.0F / 60.0F;
constexpr float kPlayerRenderWidthTiles = 40.0F / kPixelsPerTile;
constexpr float kPlayerRenderHeightTiles = 40.0F / kPixelsPerTile;

struct NetworkActor {
    bool active {false};
    opm::protocol::PlayerInfo info {};
    opm::client::net::RemotePlayerState state {};
};

class NetworkActorManager {
public:
    void clear()
    {
        for (auto& actor : actors_) {
            actor = {};
        }
    }

    void applyRoster(const std::vector<opm::protocol::PlayerInfo>& roster)
    {
        for (const auto& info : roster) {
            if (info.playerIndex >= actors_.size()) {
                continue;
            }
            auto& actor = actors_[info.playerIndex];
            actor.active = info.connected;
            actor.info = info;
        }
    }

    void applyStateUpdate(const opm::client::net::StateUpdate& update)
    {
        for (std::size_t i = 0; i < actors_.size(); ++i) {
            actors_[i].state = update.players[i];
            actors_[i].active = actors_[i].active || (update.players[i].positionX != 0.0F || update.players[i].positionY != 0.0F);
        }
        lastServerTick_ = update.serverTick;
    }

    [[nodiscard]] const std::array<NetworkActor, 2>& actors() const { return actors_; }
    [[nodiscard]] std::uint32_t lastServerTick() const { return lastServerTick_; }

private:
    std::array<NetworkActor, 2> actors_ {};
    std::uint32_t lastServerTick_ {0};
};

struct NetworkSessionContext {
    std::unique_ptr<opm::client::net::SessionClient> session {};
    bool connected {false};
    std::uint8_t localPlayerIndex {0xFFU};
    opm::engine::LevelData networkLevel {};
    NetworkActorManager actors {};
};

NetworkSessionContext gNetwork {};

struct Texture2D {
    GLuint handle {0};
    int width {0};
    int height {0};
};

struct SpriteFrame {
    int col {0};
    int row {0};
};

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

opm::engine::PlayerState toPlayerState(const opm::client::net::RemotePlayerState& state)
{
    opm::engine::PlayerState player;
    player.position.x = state.positionX;
    player.position.y = state.positionY;
    player.velocity.x = state.velocityX;
    player.velocity.y = state.velocityY;
    player.onGround = state.onGround;
    player.facingRight = state.facingRight;
    player.skidding = state.skidding;
    player.crouching = state.crouching;
    player.pSpeedActive = state.pSpeedActive;
    player.pSpeedMeter = state.pSpeedMeter;
    return player;
}

void tryLobbyFlow()
{
    if (!gNetwork.session) {
        gNetwork.session = std::make_unique<opm::client::net::SessionClient>();
    }

    std::string status;
    if (!gNetwork.session->connect("127.0.0.1", 34900U, 250U, status)) {
        std::cout << "[client] connect status: " << status << "\n";
        gNetwork.connected = false;
        return;
    }

    std::vector<opm::client::net::LobbyInfo> lobbies;
    if (!gNetwork.session->requestLobbyList(250U, lobbies, status)) {
        std::cout << "[client] lobby request status: " << status << "\n";
        gNetwork.connected = false;
        return;
    }
    std::cout << "[client] lobby request status: " << status << "\n";

    if (lobbies.empty()) {
        std::cout << "[client] lobby list empty or unavailable\n";
        gNetwork.connected = false;
        return;
    }

    std::cout << "[client] received " << lobbies.size() << " lobby entries\n";
    for (const auto& lobby : lobbies) {
        std::cout << "[client] lobby=" << lobby.name << " players=" << lobby.players
                  << "/" << lobby.capacity << "\n";
    }

    opm::client::net::JoinResult joinResult;
    const bool joined = gNetwork.session->joinLobby(lobbies.front().name, 250U, joinResult, status);
    std::cout << "[client] join lobby status: " << status
              << " success=" << (joined ? "true" : "false")
              << " player=" << static_cast<int>(joinResult.playerIndex)
              << " tickHz=" << joinResult.tickRateHz << "\n";

    if (!joined) {
        gNetwork.connected = false;
        return;
    }

    gNetwork.localPlayerIndex = joinResult.playerIndex;
    gNetwork.actors.clear();
    gNetwork.actors.applyRoster(joinResult.roster);

    opm::client::net::LevelSnapshot snapshot;
    if (gNetwork.session->receiveLevelSnapshot(250U, snapshot, status)) {
        gNetwork.networkLevel = levelFromSnapshot(snapshot);
        std::cout << "[client] level snapshot: " << snapshot.width << "x" << snapshot.height
                  << " tiles=" << snapshot.tiles.size() << " spawn=(" << snapshot.spawnX << "," << snapshot.spawnY
                  << ")\n";
    } else {
        std::cout << "[client] level snapshot status: " << status << "\n";
    }

    opm::client::net::StateUpdate update;
    if (gNetwork.session->pollStateUpdate(250U, update, status)) {
        gNetwork.actors.applyStateUpdate(update);
        const auto& p0 = update.players[0];
        std::cout << "[client] state update tick=" << update.serverTick
                  << " p0=(" << p0.positionX << "," << p0.positionY << ")"
                  << " vx=" << p0.velocityX << "\n";
    }

    gNetwork.connected = true;
    std::cout << "[client] network session is active and kept open\n";
}

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
bool loadPngRgba(const std::filesystem::path& path, std::vector<unsigned char>& pixels, int& width, int& height)
{
#ifdef OPM_CLIENT_HAS_PNG
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
int runClientWindow(const opm::assets::AssetManifest& manifest, const opm::engine::LevelData& level)
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

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Open Platformer Maker - Basic Level", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "[client] failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }

    const auto entries = opm::client::render::buildTileLayerDrawEntries(manifest, level.groundLayer, kPixelsPerTile);

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

    opm::engine::Simulation simulation;
    simulation.setLevel(level);

    bool jumpHeldLast = false;
    double previousTime = glfwGetTime();
    double accumulator = 0.0;
    float animationTime = 0.0F;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        const double now = glfwGetTime();
        double frameTime = now - previousTime;
        previousTime = now;
        if (frameTime > 0.1) {
            frameTime = 0.1;
        }
        accumulator += frameTime;

        while (accumulator >= kFixedStepSeconds) {
            const bool moveLeft =
                glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ||
                glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
            const bool moveRight =
                glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ||
                glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
            const bool jumpHeld =
                glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS ||
                glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS;
            const bool runHeld =
                glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
            const bool crouchHeld =
                glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ||
                glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;

            // Always run local simulation for input responsiveness (local prediction)
            std::array<opm::engine::InputFrame, 2> inputs {};
            inputs[0].frameIndex = simulation.state().tick;
            inputs[0].moveLeft = moveLeft;
            inputs[0].moveRight = moveRight;
            inputs[0].jumpPressed = jumpHeld && !jumpHeldLast;
            inputs[0].jumpHeld = jumpHeld;
            inputs[0].runHeld = runHeld;
            inputs[0].crouchHeld = crouchHeld;
            inputs[1].frameIndex = simulation.state().tick;
            simulation.step(inputs);

            if (gNetwork.connected && gNetwork.session) {
                std::string netStatus;
                opm::engine::InputFrame networkInput {};
                networkInput.frameIndex = gNetwork.actors.lastServerTick();
                networkInput.moveLeft = moveLeft;
                networkInput.moveRight = moveRight;
                networkInput.jumpPressed = jumpHeld && !jumpHeldLast;
                networkInput.jumpHeld = jumpHeld;
                networkInput.runHeld = runHeld;
                networkInput.crouchHeld = crouchHeld;
                (void)gNetwork.session->sendMovementInput(networkInput, netStatus);

                opm::client::net::StateUpdate update;
                if (gNetwork.session->pollStateUpdate(1U, update, netStatus)) {
                    gNetwork.actors.applyStateUpdate(update);
                }
            }

            jumpHeldLast = jumpHeld;
            animationTime += kFixedStepSeconds;
            accumulator -= kFixedStepSeconds;
        }

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            continue;
        }

        glViewport(0, 0, framebufferWidth, framebufferHeight);
        glClearColor(0.53F, 0.75F, 0.92F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(framebufferWidth), 0.0, static_cast<double>(framebufferHeight), -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // Always use local simulation for camera (local prediction)
        opm::engine::PlayerState cameraPlayer = simulation.state().players[0];

        const float playerCenterPixels = (cameraPlayer.position.x + (opm::engine::kPlayerWidthTiles * 0.5F)) * kPixelsPerTile;
        float cameraX = playerCenterPixels - static_cast<float>(framebufferWidth) * 0.35F;
        if (cameraX < 0.0F) {
            cameraX = 0.0F;
        }

        const float worldWidthPixels = static_cast<float>(level.groundLayer.width) * kPixelsPerTile;
        const float maxCameraX = std::max(0.0F, worldWidthPixels - static_cast<float>(framebufferWidth));
        if (cameraX > maxCameraX) {
            cameraX = maxCameraX;
        }

        for (const auto& entry : entries) {
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

        // Render local player from simulation (local prediction), remote players from network
        std::array<opm::engine::PlayerState, 2> playersToDraw {
            simulation.state().players[0],
            simulation.state().players[1],
        };
        if (gNetwork.connected && gNetwork.localPlayerIndex < 2) {
            // Only update remote players from network state, not the local player
            for (std::size_t i = 0; i < playersToDraw.size(); ++i) {
                if (i != gNetwork.localPlayerIndex && gNetwork.actors.actors()[i].active) {
                    playersToDraw[i] = toPlayerState(gNetwork.actors.actors()[i].state);
                }
            }
        }

        for (const auto& player : playersToDraw) {
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

        const opm::engine::PlayerState hudPlayer = playersToDraw[0];
        drawPSpeedHud(hudPlayer.pSpeedMeter, hudPlayer.pSpeedActive, framebufferWidth, framebufferHeight);

        // Update window title with connection status and ping
        if (gNetwork.session) {
            const bool netConnected = gNetwork.session->isConnected();
            const std::uint32_t pingMs = gNetwork.session->getPingMs();
            std::string title = "Open Platformer Maker - Basic Level";
            if (netConnected) {
                title += " [CONNECTED - Ping: " + std::to_string(pingMs) + "ms]";
            } else {
                title += " [DISCONNECTED]";
            }
            glfwSetWindowTitle(window, title.c_str());
        }

        glfwSwapBuffers(window);
#endif
    }

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
    const auto manifest = loadAndPrintAssetSummary(OPM_CLIENT_RESOURCE_DIR);
    const auto fallbackLevel = opm::engine::createBasicLevel(220U, 16U);
    printTileLayerStubSummary(manifest, fallbackLevel);
    tryLobbyFlow();

    const auto& runtimeLevel = (gNetwork.connected ? gNetwork.networkLevel : fallbackLevel);

    opm::engine::Simulation simulation;
    simulation.setLevel(runtimeLevel);
    std::cout << "[client] initial simulation hash: " << simulation.stateHash() << "\n";

    const auto encoded = opm::protocol::encodeMessage(
        opm::protocol::Message {
            .type = opm::protocol::MessageType::LobbyListRequest,
            .payload = opm::protocol::payloadFromString("request_lobbies")
        }
    );
    std::cout << "[client] protocol packet size for lobby request: " << encoded.size() << " bytes\n";

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)
    return runClientWindow(manifest, runtimeLevel);
#else
    std::cout << "[client] No local graphics backend available at configure-time. Running stub mode.\n";
    return 0;
#endif
}

} // namespace opm::client
