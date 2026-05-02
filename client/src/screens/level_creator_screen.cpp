#include "screens/level_creator_screen.hpp"

#include "render/asset_registry.hpp"
#include "render/sprite.hpp"

#include "opm/engine.hpp"
#include "opm/level.hpp"
#include "opm/tile_metadata.hpp"

#ifdef OPM_CLIENT_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
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

void LevelCreatorScreen::render(ScreenContext&)
{
}

void LevelCreatorScreen::renderUI(ScreenContext& ctx)
{
#ifdef OPM_CLIENT_HAS_IMGUI
    auto& session = *session_;
    auto& palette = ctx.assets.palette;
    auto& enemyRegistry = ctx.assets.enemies;
    auto& powerupRegistry = ctx.assets.powerups;

    constexpr float kTopBarH    = 38.0F;
    constexpr float kBottomBarH = 26.0F;
    constexpr float kLeftBarW   = 200.0F;
    constexpr float kRightBarW  = 260.0F;
    constexpr ImGuiWindowFlags kPanelFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize  | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings;
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
    ImGui::Begin("##creator_topbar", nullptr, kPanelFlags);
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
#endif
}

} // namespace opm::client
