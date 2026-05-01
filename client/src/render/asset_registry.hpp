#pragma once

#ifdef OPM_CLIENT_WITH_OPENGL_STUB

#include "render/sprite.hpp"
#include "render/texture.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace opm::assets { struct AssetManifest; }

namespace opm::client::render {

struct PaletteEntry {
    std::uint16_t tileIndex {0};
    std::string assetId {};
    std::string subCategory {}; // e.g. "set1", "set2" — used for palette group headers
    GLuint texture {0};
};

// Owns every long-lived GPU/CPU asset referenced by the renderer:
// tile textures, player and enemy sprite sets, and the editor palette.
// Construct empty; call load() once after the GL context exists; the
// destructor frees every GPU resource.
class AssetRegistry {
public:
    AssetRegistry() = default;
    ~AssetRegistry();

    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // Loads tile textures, player sprite set (luigi small/big), enemy
    // and powerup sprite registries, and builds the tile palette from
    // the manifest. Logs progress to stdout.
    void load(const opm::assets::AssetManifest& manifest,
              const std::filesystem::path& resourceRoot);

    // Releases every GPU resource. Called by the destructor; safe to
    // call earlier (idempotent).
    void destroy();

    std::unordered_map<std::string, GLuint> tileTextures {};
    PlayerSpriteSet playerSprites {};
    EnemyRegistry enemies {};
    EnemyRegistry powerups {};
    std::vector<PaletteEntry> palette {};
};

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB
