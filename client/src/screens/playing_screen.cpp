#include "screens/playing_screen.hpp"

#include "game/actor_manager.hpp"
#include "game/network_session.hpp"
#include "net_client.hpp"
#include "render/asset_registry.hpp"
#include "render/hud.hpp"
#include "render/sprite.hpp"
#include "render/texture.hpp"
#include "render/texture_loader.hpp"

#include "opm/engine.hpp"
#include "opm/level.hpp"
#include "opm/protocol.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#endif

#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)
#ifdef OPM_CLIENT_WITH_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#endif
#ifdef OPM_CLIENT_WITH_OPENGL_STUB
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace opm::client {

PlayingScreen::PlayingScreen(opm::client::game::GameSession& session, Callbacks callbacks)
    : session_(&session)
    , callbacks_(std::move(callbacks))
{}

ScreenTransition PlayingScreen::tick(ScreenContext& ctx, double)
{
#if defined(OPM_CLIENT_WITH_OPENGL_STUB) || defined(OPM_CLIENT_WITH_VULKAN)
    if (ctx.window == nullptr) {
        return {};
    }
    auto& session = *session_;

    // Reset frame timing while not in Playing so re-entry doesn't burst-step.
    if (session.state != opm::client::game::AppState::Playing) {
        previousTime_ = glfwGetTime();
        accumulator_ = 0.0;
        jumpHeldLast_ = false;
        timingInitialized_ = true;
        return {};
    }
    if (ctx.net == nullptr) {
        return {};
    }
    auto& net = *ctx.net;

    constexpr float kFixedStepSeconds = 1.0F / 60.0F;

    if (!timingInitialized_) {
        previousTime_ = glfwGetTime();
        timingInitialized_ = true;
    }
    const double now = glfwGetTime();
    double frameTime = now - previousTime_;
    previousTime_ = now;
    if (frameTime > 0.1) {
        frameTime = 0.1;
    }
    accumulator_ += frameTime;

    GLFWwindow* window = ctx.window;

    while (accumulator_ >= kFixedStepSeconds) {
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

        if (session.isOnline && net.session) {
            std::string netStatus;

            opm::client::net::StateUpdate update;
            bool gotTick = false;
            while (net.session->pollStateUpdate(0U, update, netStatus)) {
                net.actors.applyStateUpdate(update, net.localPlayerIndex);
                // Mirror phase / countdown / winner so PlayingScreen UI
                // (countdown overlay, win banner, auto-return) sees
                // fresh values. The lobby's onPollServer does this on
                // its side; once we're in PlayingScreen there is no
                // other writer.
                session.gamePhase = update.phase;
                session.countdownTicks = update.countdownTicks;
                session.winnerSlot = update.winnerSlot;
                session.selectedMap = update.selectedMap;
                session.selectedTiebreak = update.selectedTiebreak;
                gotTick = true;
            }
            // After draining: if the server has flipped back to PreGame
            // (i.e. post-GameOver lobby reset), drop the gameplay screen
            // and re-enter the lobby. fromEditor (Test Play) bypasses.
            if (!session.fromEditor
             && session.gamePhase == opm::protocol::GamePhase::PreGame) {
                session.state = opm::client::game::AppState::OnlineLevelSelect;
                return {};
            }

            std::vector<std::vector<opm::protocol::PlayerInfo>> rosterUpdates;
            net.session->drainRosterUpdates(rosterUpdates);
            for (const auto& roster : rosterUpdates) {
                net.actors.applyRoster(roster, net.localPlayerIndex);
            }

            std::vector<opm::client::net::LevelSnapshot> snaps;
            net.session->drainLevelSnapshots(snaps);
            if (!snaps.empty()) {
                const auto newLevel = opm::client::game::levelFromSnapshot(snaps.back());
                net.networkLevel = newLevel;
                session.activeLevel = newLevel;
                if (callbacks_.onLevelSnapshotChanged) {
                    callbacks_.onLevelSnapshotChanged(newLevel);
                }
                opm::engine::PlayerState localSpawnState;
                localSpawnState.position.x = newLevel.spawnX;
                localSpawnState.position.y = newLevel.spawnY;
                net.actors.updateLocalState(localSpawnState);
                std::cout << "[client] level switched mid-session: "
                          << newLevel.foliage.width << "x" << newLevel.foliage.height << "\n";
            }

            net.session->sendPingIfDue(500U);

            if (gotTick) {
                opm::engine::InputFrame networkInput {};
                networkInput.frameIndex = net.actors.lastServerTick();
                networkInput.moveLeft = moveLeft;
                networkInput.moveRight = moveRight;
                networkInput.jumpPressed = jumpHeld && !jumpHeldLast_;
                networkInput.jumpHeld = jumpHeld;
                networkInput.runHeld = runHeld;
                networkInput.crouchHeld = crouchHeld;
                (void)net.session->sendMovementInput(networkInput, netStatus);
            }
        } else {
            // Offline single-player: run local simulation.
            std::array<opm::engine::InputFrame, 2> inputs {};
            inputs[0].frameIndex = session.simulation.state().tick;
            inputs[0].moveLeft = moveLeft;
            inputs[0].moveRight = moveRight;
            inputs[0].jumpPressed = jumpHeld && !jumpHeldLast_;
            inputs[0].jumpHeld = jumpHeld;
            inputs[0].runHeld = runHeld;
            inputs[0].crouchHeld = crouchHeld;
            inputs[1].frameIndex = session.simulation.state().tick;
            session.simulation.step(inputs);
            net.actors.updateLocalState(session.simulation.state().players[0]);
            net.actors.setWorldActors(session.simulation.state().actors);
        }

        jumpHeldLast_ = jumpHeld;
        animationTime_ += kFixedStepSeconds;
        accumulator_ -= kFixedStepSeconds;
    }
#else
    (void)ctx;
#endif
    return {};
}

void PlayingScreen::colorForAsset(const std::string& assetId, float& r, float& g, float& b)
{
    const auto h = static_cast<unsigned long long>(std::hash<std::string> {}(assetId));
    r = 0.2F + static_cast<float>((h >> 0U) & 0xFFU) / 510.0F;
    g = 0.2F + static_cast<float>((h >> 8U) & 0xFFU) / 510.0F;
    b = 0.2F + static_cast<float>((h >> 16U) & 0xFFU) / 510.0F;
}

void PlayingScreen::render(ScreenContext& ctx)
{
#ifdef OPM_CLIENT_WITH_OPENGL_STUB
    if (ctx.net == nullptr) {
        return;
    }
    auto& session = *session_;
    auto& net = *ctx.net;
    auto& assets = ctx.assets;
    auto& textures = assets.tileTextures;
    auto& playerSpriteSet = assets.playerSprites;
    auto& enemyRegistry = assets.enemies;
    auto& powerupRegistry = assets.powerups;
    const int framebufferWidth = ctx.framebufferWidth;
    const int framebufferHeight = ctx.framebufferHeight;
    const float animationTime = animationTime_;

    using opm::client::render::PlayerSprite;
    using opm::client::render::PlayerFrameSelection;
    using opm::client::render::EnemyRegistry;
    using opm::client::render::EnemySprite;
    using opm::client::render::Texture2D;
    using opm::client::render::drawTextureQuad;
    using opm::client::render::drawDebugPlayerBody;
    using opm::client::render::drawPSpeedHud;
    using opm::client::render::selectPlayerFrame;
    using opm::client::render::selectEnemyFrame;

    constexpr float kPixelsPerTile = 24.0F;
    constexpr float kPlayerRenderWidthTiles = 40.0F / kPixelsPerTile;
    constexpr float kPlayerRenderHeightTiles = 40.0F / kPixelsPerTile;

    opm::engine::PlayerState cameraPlayer {};
    if (!net.actors.actors().empty()) {
        cameraPlayer = net.actors.actors().front().state;
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
    drawTileLayer(session.entries.foliage, 1.0F);

    const float bodyWidthPixels = opm::engine::kPlayerWidthTiles * kPixelsPerTile;
    const float fallbackWidthPixels  = kPlayerRenderWidthTiles  * kPixelsPerTile;
    const float fallbackHeightPixels = kPlayerRenderHeightTiles * kPixelsPerTile;

    // Anchors a quad of pixel size (texW, texH) at the bottom-center
    // of the body (so feet sit on `bodyBottom` and the sprite stays
    // centered horizontally on `bodyLeft + bodyWidth/2`).
    const auto anchorBottomCenter = [](float bodyLeftWorld, float bodyBottomWorld,
                                       float bodyW, float texW, float texH,
                                       float& x0, float& y0, float& x1, float& y1) {
        x0 = bodyLeftWorld - (texW - bodyW) * 0.5F;
        y0 = bodyBottomWorld;
        x1 = x0 + texW;
        y1 = y0 + texH;
    };

    for (const auto& actor : net.actors.actors()) {
        const auto& player = actor.state;
        const float bodyLeftWorld   = (player.position.x * kPixelsPerTile) - cameraX;
        const float bodyBottomWorld = (player.position.y * kPixelsPerTile) - cameraY;

        // Two visual i-frame effects:
        //   * Power-up transition: draw the *opposite* style every
        //     few frames so the upgrade alternates Small <-> Big.
        //   * Post-damage i-frames: skip drawing the sprite every
        //     other ~3-frame slot so the player blinks.
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
            continue;
        }
        const PlayerSprite& chosen = playerSpriteSet.forStyle(styleToDraw);
        const PlayerFrameSelection sel = selectPlayerFrame(chosen, player, animationTime);
        if (sel.tex != nullptr) {
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

    // Render world actors (enemies/NPCs).
    for (const auto& a : net.actors.worldActors()) {
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

    drawPSpeedHud(cameraPlayer.pSpeedMeter, cameraPlayer.pSpeedActive,
                  framebufferWidth, framebufferHeight);
#else
    (void)ctx;
#endif
}

void PlayingScreen::renderUI(ScreenContext& ctx)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    auto& session = *session_;

    // ===== Test Play HUD =====
    if (session.fromEditor) {
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
            session.state = opm::client::game::AppState::LevelCreator;
        }
        ImGui::End();
    }

    // Phase-driven overlays only apply during online play (the editor
    // test-play path bypasses the lobby state machine entirely).
    if (session.fromEditor || !session.isOnline) {
        return;
    }

    constexpr float kTicksPerSecond = 60.0F;
    const float secondsRemaining =
        static_cast<float>(session.countdownTicks) / kTicksPerSecond;

    auto centeredOverlay = [&](const char* tag, const char* text) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F,
                   vp->WorkPos.y + vp->WorkSize.y * 0.40F),
            ImGuiCond_Always, ImVec2(0.5F, 0.5F));
        ImGui::SetNextWindowSize(ImVec2(0.0F, 0.0F));
        ImGui::Begin(tag, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
        ImGui::SetWindowFontScale(2.4F);
        ImGui::TextUnformatted(text);
        ImGui::SetWindowFontScale(1.0F);
        ImGui::End();
    };

    char buf[160];

    // ===== RoundStarting countdown =====
    if (session.gamePhase == opm::protocol::GamePhase::RoundStarting) {
        const int s = std::max(1, static_cast<int>(std::ceil(secondsRemaining)));
        std::snprintf(buf, sizeof(buf), "GET READY  -  %d", s);
        centeredOverlay("##roundstart_overlay", buf);
    }

    // ===== GameOver winner banner =====
    if (session.gamePhase == opm::protocol::GamePhase::GameOver) {
        const int s = std::max(0, static_cast<int>(std::ceil(secondsRemaining)));
        const bool weWon =
            ctx.net != nullptr && session.winnerSlot != 0xFFFFU &&
            ctx.net->actors.localActor() != nullptr &&
            ctx.net->actors.localActor()->serverIndex == session.winnerSlot;
        if (weWon) {
            std::snprintf(buf, sizeof(buf),
                "YOU WIN!\nReturning to lobby in %ds", s);
        } else if (session.winnerSlot != 0xFFFFU) {
            std::snprintf(buf, sizeof(buf),
                "PLAYER #%u WINS\nReturning to lobby in %ds",
                static_cast<unsigned>(session.winnerSlot), s);
        } else {
            std::snprintf(buf, sizeof(buf),
                "ROUND OVER\nReturning to lobby in %ds", s);
        }
        centeredOverlay("##gameover_overlay", buf);
    }
#endif
}

} // namespace opm::client
