#include "screens/level_creator_screen.hpp"

#include "render/asset_registry.hpp"
#include "render/sprite.hpp"
#include "render/texture.hpp"
#include "render/texture_loader.hpp"
#include "render/ui_widgets.hpp"

#include "opm/engine.hpp"
#include "opm/level.hpp"
#include "opm/tile_metadata.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#endif

#ifdef OPM_CLIENT_WITH_OPENGL_STUB
#ifdef OPM_CLIENT_WITH_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace opm::client {

LevelCreatorScreen::LevelCreatorScreen(opm::client::game::GameSession& session, Callbacks callbacks)
    : session_(&session)
    , callbacks_(std::move(callbacks))
{}

ScreenTransition LevelCreatorScreen::tick(ScreenContext&, double)
{
    return {};
}

opm::engine::TileLayer& LevelCreatorScreen::layerByEnum(opm::client::game::EditorLayer which)
{
    using opm::client::game::EditorLayer;
    switch (which) {
        case EditorLayer::Background: return session_->editor.level.background;
        case EditorLayer::Foreground: return session_->editor.level.foreground;
        case EditorLayer::Foliage:    [[fallthrough]];
        default:                      return session_->editor.level.foliage;
    }
}

std::vector<opm::client::render::TileDrawEntry>& LevelCreatorScreen::layerEntriesByEnum(
    opm::client::game::EditorLayer which)
{
    using opm::client::game::EditorLayer;
    switch (which) {
        case EditorLayer::Background: return session_->editor.entries.background;
        case EditorLayer::Foreground: return session_->editor.entries.foreground;
        case EditorLayer::Foliage:    [[fallthrough]];
        default:                      return session_->editor.entries.foliage;
    }
}

void LevelCreatorScreen::render(ScreenContext& ctx)
{
#ifdef OPM_CLIENT_WITH_OPENGL_STUB
    if (ctx.window == nullptr) {
        return;
    }
    auto& session = *session_;
    auto& assets = ctx.assets;
    auto& textures = assets.tileTextures;
    auto& enemyRegistry = assets.enemies;
    auto& powerupRegistry = assets.powerups;
    GLFWwindow* window = ctx.window;
    const int framebufferWidth = ctx.framebufferWidth;
    const int framebufferHeight = ctx.framebufferHeight;

    using opm::client::game::EditorLayer;
    using opm::client::render::EnemyRegistry;
    using opm::client::render::EnemySprite;
    using opm::client::render::Texture2D;
    using opm::client::render::drawTextureQuad;

    constexpr float kPixelsPerTile = 24.0F;
    constexpr float kPlayerRenderWidthTiles = 40.0F / kPixelsPerTile;
    constexpr float kPlayerRenderHeightTiles = 40.0F / kPixelsPerTile;

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
                const float zoomFactor = (wheel > 0.0F) ? 1.10F : (1.0F / 1.10F);
                session.editor.zoom = std::clamp(session.editor.zoom * zoomFactor, 0.25F, 4.0F);
                const float newPixelsPerTile = kPixelsPerTile * session.editor.zoom;
                session.editor.cameraX = worldTileX * newPixelsPerTile - cursorPx;
                session.editor.cameraY = worldTileY * newPixelsPerTile - cursorPy;
            }
        }
    }
#endif

    // Re-derive after zoom changes (cameraX/Y, editorPixelsPerTile may
    // have shifted). Local copies follow:
    const float editorPixelsPerTileNow = kPixelsPerTile * session.editor.zoom;
    (void)editorPixelsPerTileNow; // already captured below by re-reading

    // Middle-mouse drag pan.
    const bool mmbDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    int winW = 0;
    int winH = 0;
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
    // Shadow layer: rendered before all tiles so terrain naturally occludes
    // shadow fragments that fall inside solid geometry.
    {
        constexpr float kShadowAlpha =  0.45F;
        constexpr float kShadowDX    =  3.0F;
        constexpr float kShadowDY    = -4.0F;
        const float editorScale = editorPixelsPerTile / kPixelsPerTile;
        const float shadowBodyW = opm::engine::kPlayerWidthTiles * editorPixelsPerTile;
        const float fallbackW   = kPlayerRenderWidthTiles  * editorPixelsPerTile;
        const float fallbackH   = kPlayerRenderHeightTiles * editorPixelsPerTile;
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
            const float ax0 = bodyLeft - (drawW - shadowBodyW) * 0.5F;
            const float ay0 = bodyBottom;
            const float ax1 = ax0 + drawW;
            const float ay1 = ay0 + drawH;
            if (tex != nullptr) {
                drawTextureQuadTinted(*tex, false,
                    ax0 + kShadowDX * zoomScale, ay0 + kShadowDY * zoomScale,
                    ax1 + kShadowDX * zoomScale, ay1 + kShadowDY * zoomScale,
                    0.0F, 0.0F, 0.0F, kShadowAlpha);
            }
        }
        // Safe-zone shadow: dark offset dashes drawn before tiles.
        {
            const auto z = opm::engine::computeSpawnSafeZone(session.editor.level);
            const float x0 = z.minX * editorPixelsPerTile - cameraX + kShadowDX * zoomScale;
            const float y0 = z.minY * editorPixelsPerTile - cameraY + kShadowDY * zoomScale;
            const float x1 = z.maxX * editorPixelsPerTile - cameraX + kShadowDX * zoomScale;
            const float y1 = z.maxY * editorPixelsPerTile - cameraY + kShadowDY * zoomScale;
            glBindTexture(GL_TEXTURE_2D, 0);
            glColor4f(0.0F, 0.0F, 0.0F, 0.5F);
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
    }
    drawEditorLayer(session.editor.entries.background, layerAlpha(EditorLayer::Background));
    drawEditorLayer(session.editor.entries.foliage,    layerAlpha(EditorLayer::Foliage));
    // Safe-zone colored dotted outline — visible over tile layers.
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
    // them apart at a glance.
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
                            if (callbacks_.onRebuildActiveLayer) {
                                callbacks_.onRebuildActiveLayer();
                            }
                        }
                    }
                } else if (rightDown) {
                    if (activeLayer.tileIndices[flat] != 0U) {
                        activeLayer.tileIndices[flat] = 0U;
                        session.editor.dirty = true;
                        if (callbacks_.onRebuildActiveLayer) {
                            callbacks_.onRebuildActiveLayer();
                        }
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
                        if (callbacks_.onRebuildActiveLayer) {
                            callbacks_.onRebuildActiveLayer();
                        }
                    }
                }
                session.editor.rotateKeyPrev = rNow;
            }
        }
    }
#else
    (void)ctx;
#endif
}

void LevelCreatorScreen::renderUI(ScreenContext& ctx)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    auto& session = *session_;
    auto& palette = ctx.assets.palette;
    auto& enemyRegistry = ctx.assets.enemies;
    auto& powerupRegistry = ctx.assets.powerups;

    // Chrome bars use a tighter WindowPadding (set per-Begin below) so
    // a single row of buttons / text centers inside them. Heights chosen
    // to fit content snugly: top bar holds buttons (text + 2*FramePadding.y
    // = ~26px), bottom bar is just text (~14px).
    constexpr float kTopBarH    = 38.0F;
    constexpr float kBottomBarH = 22.0F;
    constexpr ImVec2 kTopBarPadding    {10.0F, 6.0F};
    constexpr ImVec2 kStatusBarPadding {10.0F, 4.0F};
    constexpr float kMinSidebarW = 140.0F;
    constexpr float kMaxSidebarW = 480.0F;
    // Anchored panels: not movable, not collapsible, no saved settings,
    // and no scrollbar on edge bars (they're a single fixed line).
    constexpr ImGuiWindowFlags kFixedPanelFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize  | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
    // Side panels keep NoMove but allow ImGui's resize grip so the user
    // can drag them wider/narrower; widths persist on the editor.
    constexpr ImGuiWindowFlags kSidePanelFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    auto& leftBarW  = session.editor.leftSidebarWidth;
    auto& rightBarW = session.editor.rightSidebarWidth;
    leftBarW  = std::clamp(leftBarW,  kMinSidebarW, kMaxSidebarW);
    rightBarW = std::clamp(rightBarW, kMinSidebarW, kMaxSidebarW);
    const float fbW = static_cast<float>(ctx.framebufferWidth);
    const float fbH = static_cast<float>(ctx.framebufferHeight);

    using opm::client::game::EditorLayer;
    using opm::client::render::EnemyRegistry;

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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, kTopBarPadding);
    ImGui::Begin("##creator_topbar", nullptr, kFixedPanelFlags);
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0F);
        ImGui::InputText("##levelname", session.editor.nameInput, sizeof(session.editor.nameInput));
        ImGui::SameLine();
        if (ImGui::Button("Save to server", ImVec2(140.0F, 0.0F))) {
            callbacks_.onSaveLevel();
        }
        ImGui::SameLine();
        if (ImGui::Button("Test Play", ImVec2(100.0F, 0.0F))) {
            callbacks_.onTestPlay();
        }
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(70.0F, 0.0F))) {
            session.state = opm::client::game::AppState::MainMenu;
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
    ImGui::PopStyleVar(); // kTopBarPadding

    // ===== Left sidebar: layer + markers + size =====
    ImGui::SetNextWindowPos(ImVec2(0.0F, kTopBarH));
    ImGui::SetNextWindowSize(ImVec2(leftBarW, fbH - kTopBarH - kBottomBarH));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(kMinSidebarW, fbH - kTopBarH - kBottomBarH),
        ImVec2(kMaxSidebarW, fbH - kTopBarH - kBottomBarH));
    ImGui::Begin("##creator_left", nullptr, kSidePanelFlags);
    leftBarW = ImGui::GetWindowSize().x;
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
                callbacks_.onResize(session.editor.resizeWidth, session.editor.resizeHeight);
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
    ImGui::SetNextWindowPos(ImVec2(fbW - rightBarW, kTopBarH));
    ImGui::SetNextWindowSize(ImVec2(rightBarW, fbH - kTopBarH - kBottomBarH));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(kMinSidebarW, fbH - kTopBarH - kBottomBarH),
        ImVec2(kMaxSidebarW, fbH - kTopBarH - kBottomBarH));
    ImGui::Begin("##creator_right", nullptr, kSidePanelFlags);
    rightBarW = ImGui::GetWindowSize().x;
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
        // Collision-mask inspector toggle: small chevron on the row
        // with the TILES header. Chevron points LEFT when closed (hint
        // that the panel will appear in that direction); RIGHT when
        // open (click to collapse it back).
        {
            const ImGuiDir dir = session.editor.collisionInspectorOpen
                ? ImGuiDir_Right : ImGuiDir_Left;
            if (ImGui::ArrowButton("##collision_toggle", dir)) {
                session.editor.collisionInspectorOpen = !session.editor.collisionInspectorOpen;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s collision-mask inspector",
                    session.editor.collisionInspectorOpen ? "Hide" : "Show");
            }
            ImGui::SameLine();
        }
        ImGui::TextDisabled("TILES");
        ImGui::SameLine();
        ImGui::TextColored(layerColor, "[%s]", layerLabel);
        ImGui::Separator();

        {
            // Eraser button: use Ghost variant, full-width, 32px height
            ImGui::PushID("##eraser");
            const bool clicked = opm::client::render::OpmButtonGhost(
                "Eraser", ImVec2(-1.0F, 32.0F));
            if (clicked) {
                session.editor.selectedTile = 0U;
                session.editor.placingSpawn = false;
                session.editor.placingGoal = false;
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        constexpr float thumbSize = 64.0F;
        // 2-column MM2-style grid
        const int cols = 2;
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
                    opm::client::render::OpmSectionHeader(currentPaletteSet.c_str());
                }
            }
            if (columnCounter > 0) {
                ImGui::SameLine();
            }
            ImGui::PushID(static_cast<int>(entry.tileIndex));
            const bool selected = session.editor.selectedTile == entry.tileIndex
                && !session.editor.placingSpawn && !session.editor.placingGoal;
            
            // Use OpmIconButton for MM2-style appearance.
            // Note: OpmIconButton expects a valid ImTextureID; cast as before.
            const ImTextureID texId = entry.texture != 0U
                ? (ImTextureID)(intptr_t)entry.texture
                : ImTextureID_Invalid;
            if (opm::client::render::OpmIconButton(texId, "", ImVec2(thumbSize, thumbSize), selected)) {
                session.editor.selectedTile = entry.tileIndex;
                session.editor.placingSpawn = false;
                session.editor.placingGoal = false;
            }
            ImGui::PopID();
            if (++columnCounter >= cols) {
                columnCounter = 0;
            }
        }
    }
    ImGui::End();

    // ===== Collision inspector =====
    const bool tileLayerActive =
        session.editor.activeLayer == EditorLayer::Background
        || session.editor.activeLayer == EditorLayer::Foliage
        || session.editor.activeLayer == EditorLayer::Foreground;
    const bool inspectorOpen = session.editor.collisionInspectorOpen
        && tileLayerActive
        && session.editor.selectedTile != 0U
        && !session.editor.placingSpawn
        && !session.editor.placingGoal;
    if (inspectorOpen) {
        constexpr float kInspectorW = 240.0F;
        ImGui::SetNextWindowPos(
            ImVec2(fbW - rightBarW - kInspectorW, kTopBarH));
        ImGui::SetNextWindowSize(
            ImVec2(kInspectorW, fbH - kTopBarH - kBottomBarH));
        ImGui::Begin("##creator_collision", nullptr, kFixedPanelFlags);
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, kStatusBarPadding);
    ImGui::Begin("##creator_status", nullptr, kFixedPanelFlags);
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
    ImGui::PopStyleVar(); // kStatusBarPadding
#endif
}

} // namespace opm::client
