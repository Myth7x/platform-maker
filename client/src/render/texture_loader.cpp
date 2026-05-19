#include "render/texture_loader.hpp"

#ifdef OPM_CLIENT_WITH_OPENGL_STUB

#include "opm/assets.hpp"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
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
#endif

#include <cstddef>
#include <cstdio>

namespace opm::client::render {

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

Texture2D uploadTextureRgba(const std::vector<unsigned char>& rgba, const int width, const int height, const GLenum filter)
{
    if (rgba.empty() || width <= 0 || height <= 0) {
        return {};
    }

    Texture2D texture;
    glGenTextures(1, &texture.handle);
    glBindTexture(GL_TEXTURE_2D, texture.handle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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

bool loadTextureFromPath(const std::filesystem::path& path, Texture2D& out)
{
    std::vector<unsigned char> rgba;
    int w = 0;
    int h = 0;
    if (!loadPngRgba(path, rgba, w, h)) {
        return false;
    }
    out = uploadTextureRgba(rgba, w, h);
    return out.handle != 0;
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

        const Texture2D texture = uploadTextureRgba(rgba, width, height, GL_LINEAR);
        if (texture.handle != 0) {
            textures[record.id] = texture.handle;
        }
    }

    return textures;
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

void drawTextureQuad(const Texture2D& tex,
                     const bool flipX,
                     const float x0, const float y0,
                     const float x1, const float y1,
                     const bool flipY)
{
    if (tex.handle == 0) {
        return;
    }
    const float u0 = flipX ? 1.0F : 0.0F;
    const float u1 = flipX ? 0.0F : 1.0F;
    const float v0 = flipY ? 0.0F : 1.0F;
    const float v1 = flipY ? 1.0F : 0.0F;
    glBindTexture(GL_TEXTURE_2D, tex.handle);
    glColor3f(1.0F, 1.0F, 1.0F);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x0, y0);
    glTexCoord2f(u1, v0); glVertex2f(x1, y0);
    glTexCoord2f(u1, v1); glVertex2f(x1, y1);
    glTexCoord2f(u0, v1); glVertex2f(x0, y1);
    glEnd();
}

void drawTextureQuadTinted(const Texture2D& tex,
                           const bool flipX,
                           const float x0, const float y0,
                           const float x1, const float y1,
                           const float r, const float g, const float b, const float a,
                           const bool flipY)
{
    if (tex.handle == 0) {
        return;
    }
    const float u0 = flipX ? 1.0F : 0.0F;
    const float u1 = flipX ? 0.0F : 1.0F;
    const float v0 = flipY ? 0.0F : 1.0F;
    const float v1 = flipY ? 1.0F : 0.0F;
    glBindTexture(GL_TEXTURE_2D, tex.handle);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x0, y0);
    glTexCoord2f(u1, v0); glVertex2f(x1, y0);
    glTexCoord2f(u1, v1); glVertex2f(x1, y1);
    glTexCoord2f(u0, v1); glVertex2f(x0, y1);
    glEnd();
}

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB
