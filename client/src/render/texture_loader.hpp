#pragma once

#ifdef OPM_CLIENT_WITH_OPENGL_STUB

#include "render/texture.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace opm::assets { struct AssetManifest; }

namespace opm::client::render {

// Decodes a PNG into RGBA8 pixels. Returns false on any failure (file
// missing, decode error, or no PNG backend compiled in).
[[nodiscard]] bool loadPngRgba(const std::filesystem::path& path,
                               std::vector<unsigned char>& pixels,
                               int& width,
                               int& height);

// Uploads RGBA8 pixels to a GL texture. Returns a default-constructed
// Texture2D (handle == 0) on bad input.
Texture2D uploadTextureRgba(const std::vector<unsigned char>& rgba,
                            int width,
                            int height,
                            GLenum filter = GL_NEAREST);

// Convenience: loadPngRgba + uploadTextureRgba. Returns false if either
// step fails.
[[nodiscard]] bool loadTextureFromPath(const std::filesystem::path& path, Texture2D& out);

// Uploads every "Tiles" record in the manifest as a GL texture and
// returns an asset-id → handle map.
std::unordered_map<std::string, GLuint> buildTileTextureMap(const opm::assets::AssetManifest& manifest);

// Frees every nonzero handle in `textures` and clears the map.
void destroyTileTextures(std::unordered_map<std::string, GLuint>& textures);

// Draws a textured quad covering [x0,y0]-[x1,y1] using fixed-pipeline
// GL. `flipX` mirrors the texture horizontally.
void drawTextureQuad(const Texture2D& tex,
                     bool flipX,
                     float x0, float y0,
                     float x1, float y1,
                     bool flipY = false);

// Like drawTextureQuad but multiplies each fragment by (r, g, b, a).
// Set r=g=b=0 to render a black silhouette (useful for drop-shadow passes).
void drawTextureQuadTinted(const Texture2D& tex,
                           bool flipX,
                           float x0, float y0,
                           float x1, float y1,
                           float r, float g, float b, float a,
                           bool flipY = false);

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB
