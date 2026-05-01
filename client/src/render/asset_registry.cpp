#include "render/asset_registry.hpp"

#ifdef OPM_CLIENT_WITH_OPENGL_STUB

#include "render/texture_loader.hpp"

#include "opm/assets.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <string_view>
#include <system_error>

namespace opm::client::render {
namespace {

void logRegistry(const char* label, const EnemyRegistry& r)
{
    if (r.size() > 0) {
        std::cout << "[client] " << label << " registry: " << r.size() << " kind(s):";
        for (std::size_t i = 0; i < r.size(); ++i) {
            std::cout << " [" << i << "]=" << r.names[i];
        }
        std::cout << "\n";
    } else {
        std::cout << "[client] " << label << " registry empty\n";
    }
}

void deleteTextureList(std::vector<Texture2D>& list)
{
    for (auto& t : list) {
        if (t.handle != 0) {
            glDeleteTextures(1, &t.handle);
            t.handle = 0;
        }
    }
}

void deleteAnimClip(AnimClip& clip)
{
    deleteTextureList(clip.framesRight);
    deleteTextureList(clip.framesLeft);
}

void deletePlayerSprite(PlayerSprite& ps)
{
    deleteAnimClip(ps.idle);
    deleteAnimClip(ps.crouch);
    deleteAnimClip(ps.walkNormal);
    deleteAnimClip(ps.walkNormalTurnaround);
    deleteAnimClip(ps.airNormal);
    deleteAnimClip(ps.walkPSpeed);
    deleteAnimClip(ps.walkPSpeedTurnaround);
    deleteAnimClip(ps.airPSpeed);
}

} // namespace

AssetRegistry::~AssetRegistry()
{
    destroy();
}

void AssetRegistry::load(const opm::assets::AssetManifest& manifest,
                         const std::filesystem::path& resourceRoot)
{
    tileTextures = buildTileTextureMap(manifest);
    std::cout << "[client] loaded tile textures: " << tileTextures.size() << "\n";

    const std::filesystem::path actorsRoot = resourceRoot / "Actors";
    playerSprites.small = loadPlayerSprite(actorsRoot / "player" / "luigi" / "small");
    playerSprites.big   = loadPlayerSprite(actorsRoot / "player" / "luigi" / "big");
    std::cout << "[client] player sprites — small: "
              << (playerSprites.small.ready ? "ready" : "missing")
              << ", big: "
              << (playerSprites.big.ready ? "ready" : "missing")
              << "\n";

    // Two parallel visual registries: enemies and powerups. Both use the
    // same sprite-directory scanner; the simulation routes actors to the
    // right registry via `ActorState::category`.
    enemies  = loadEnemyRegistry(actorsRoot / "enemies");
    powerups = loadEnemyRegistry(actorsRoot / "powerup");
    logRegistry("enemy", enemies);
    logRegistry("powerup", powerups);

    // Build the tile palette: maps tile index (1..N) to its texture handle.
    palette.clear();
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
        const auto it = tileTextures.find(record.id);
        const GLuint handle = (it != tileTextures.end()) ? it->second : 0U;
        palette.push_back(PaletteEntry {
            .tileIndex = tileIndex,
            .assetId = record.id,
            .subCategory = record.subCategory,
            .texture = handle,
        });
    }
    std::sort(palette.begin(), palette.end(),
        [](const PaletteEntry& a, const PaletteEntry& b) { return a.tileIndex < b.tileIndex; });
    std::cout << "[client] palette entries: " << palette.size() << "\n";
}

void AssetRegistry::destroy()
{
    deletePlayerSprite(playerSprites.small);
    deletePlayerSprite(playerSprites.big);
    for (auto& es : enemies.sprites) {
        deleteTextureList(es.frames);
    }
    for (auto& es : powerups.sprites) {
        deleteTextureList(es.frames);
    }
    destroyTileTextures(tileTextures);
    palette.clear();
}

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB
