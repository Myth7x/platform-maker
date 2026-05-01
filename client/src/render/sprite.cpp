#include "render/sprite.hpp"

#ifdef OPM_CLIENT_WITH_OPENGL_STUB

#include "render/texture_loader.hpp"
#include "sprite_config.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace opm::client::render {
namespace {

// Loads `<dir>/0.png`, `<dir>/1.png`, ... in numeric order. Stops at the
// first missing index.
std::vector<Texture2D> loadNumberedFrames(const std::filesystem::path& dir)
{
    std::vector<Texture2D> out;
    if (!std::filesystem::is_directory(dir)) {
        return out;
    }
    for (int i = 0; i < 64; ++i) {
        const auto path = dir / (std::to_string(i) + ".png");
        Texture2D tex;
        if (!std::filesystem::exists(path)) {
            break;
        }
        if (loadTextureFromPath(path, tex)) {
            out.push_back(tex);
        }
    }
    return out;
}

// PNGs whose stems are not necessarily consecutive integers
// (e.g. koopa_00.png, koopa_01.png), sorted by name.
std::vector<Texture2D> loadAllPngsSorted(const std::filesystem::path& dir)
{
    std::vector<Texture2D> out;
    if (!std::filesystem::is_directory(dir)) {
        return out;
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".png") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& p : paths) {
        Texture2D tex;
        if (loadTextureFromPath(p, tex)) {
            out.push_back(tex);
        }
    }
    return out;
}

AnimClip loadAnimClip(const std::filesystem::path& base)
{
    AnimClip clip;
    clip.framesRight = loadNumberedFrames(base / "right");
    clip.framesLeft = loadNumberedFrames(base / "left");
    return clip;
}

std::vector<Texture2D> loadFramesFromPaths(const std::filesystem::path& styleDir,
                                           const std::vector<std::string>& relativePaths)
{
    std::vector<Texture2D> out;
    out.reserve(relativePaths.size());
    for (const auto& rel : relativePaths) {
        const auto path = styleDir / rel;
        Texture2D tex;
        if (loadTextureFromPath(path, tex)) {
            out.push_back(tex);
        }
    }
    return out;
}

// Pick the AnimDef for `name` from the parsed config and turn it into an
// AnimClip. If the name isn't in the config, fall back to scanning the
// conventional folder layout (`<convFolder>/{left,right}/<n>.png`) at
// `defaultFps`.
AnimClip resolveClip(const std::filesystem::path& styleDir,
                     const opm::client::sprite::PlayerStyleConfig& cfg,
                     const std::string& name,
                     const std::filesystem::path& convFolder,
                     float defaultFps)
{
    AnimClip clip;
    const auto it = cfg.animations.find(name);
    if (it != cfg.animations.end()) {
        clip.framesRight = loadFramesFromPaths(styleDir, it->second.rightPaths);
        clip.framesLeft  = loadFramesFromPaths(styleDir, it->second.leftPaths);
        clip.fps = it->second.fps;
        clip.heightTiles = it->second.heightTiles;
        return clip;
    }
    clip = loadAnimClip(styleDir / convFolder);
    clip.fps = defaultFps;
    return clip;
}

} // namespace

PlayerSprite loadPlayerSprite(const std::filesystem::path& styleDir)
{
    opm::client::sprite::PlayerStyleConfig cfg;
    std::string err;
    if (!opm::client::sprite::loadPlayerStyleConfig(styleDir, cfg, err)) {
        std::cout << "[client] config.json error in '" << styleDir
                  << "': " << err << " (using directory convention)\n";
        cfg = {};
    }

    PlayerSprite ps;
    ps.idle                  = resolveClip(styleDir, cfg, "idle",                   "idle",                                 0.0F);
    ps.crouch                = resolveClip(styleDir, cfg, "crouch",                 "crouch",                               0.0F);
    ps.walkNormal            = resolveClip(styleDir, cfg, "walk_normal",            "movement/normal",                      10.0F);
    ps.walkNormalTurnaround  = resolveClip(styleDir, cfg, "walk_normal_turnaround", "movement/normal/turnaround",            0.0F);
    ps.airNormal             = resolveClip(styleDir, cfg, "air_normal",             "movement/normal/air",                   0.0F);
    ps.walkPSpeed            = resolveClip(styleDir, cfg, "walk_pspeed",            "movement/pspeed",                      14.0F);
    ps.walkPSpeedTurnaround  = resolveClip(styleDir, cfg, "walk_pspeed_turnaround", "movement/pspeed/turnaround",            0.0F);
    ps.airPSpeed             = resolveClip(styleDir, cfg, "air_pspeed",             "movement/pspeed/air",                   0.0F);
    ps.ready = !ps.idle.empty() || !ps.walkNormal.empty();
    return ps;
}

EnemySprite loadEnemySprite(const std::filesystem::path& dir)
{
    EnemySprite es;
    // Frame discovery cascade:
    //   1. dir/0.png, dir/1.png, ...        (numbered convention)
    //   2. dir/*.png sorted by name         (e.g. koopa_00.png, koopa_01.png)
    //   3. dir/frames/0.png, ...            (numbered, in frames/ subdir)
    //   4. dir/frames/*.png sorted by name  (e.g. Mushroom_1.png)
    es.frames = loadNumberedFrames(dir);
    if (es.frames.empty()) {
        es.frames = loadAllPngsSorted(dir);
    }
    if (es.frames.empty()) {
        const auto framesDir = dir / "frames";
        es.frames = loadNumberedFrames(framesDir);
        if (es.frames.empty()) {
            es.frames = loadAllPngsSorted(framesDir);
        }
    }
    es.ready = !es.frames.empty();

    opm::client::sprite::EnemyConfig cfg;
    std::string err;
    if (!opm::client::sprite::loadEnemyConfig(dir, cfg, err)) {
        std::cout << "[client] enemy config.json error in '" << dir
                  << "': " << err << " (using defaults)\n";
    } else {
        es.heightTiles = cfg.heightTiles;
        es.fps = cfg.fps;
    }
    return es;
}

EnemyRegistry loadEnemyRegistry(const std::filesystem::path& enemiesRoot)
{
    EnemyRegistry reg;
    if (!std::filesystem::is_directory(enemiesRoot)) {
        return reg;
    }
    std::vector<std::filesystem::path> dirs;
    for (const auto& entry : std::filesystem::directory_iterator(enemiesRoot)) {
        if (entry.is_directory()) {
            dirs.push_back(entry.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
        EnemySprite es = loadEnemySprite(d);
        if (!es.ready) {
            continue;
        }
        reg.names.push_back(d.filename().string());
        reg.sprites.push_back(std::move(es));
    }
    return reg;
}

PlayerFrameSelection selectPlayerFrame(const PlayerSprite& sprite,
                                       const opm::engine::PlayerState& player,
                                       const float animTime)
{
    PlayerFrameSelection out;
    if (!sprite.ready) {
        return out;
    }

    const bool right = player.facingRight;
    const AnimClip* clip = nullptr;
    int frameIndex = 0;

    if (player.crouching) {
        clip = &sprite.crouch;
    } else if (!player.onGround) {
        clip = player.pSpeedActive ? &sprite.airPSpeed : &sprite.airNormal;
    } else if (player.skidding) {
        clip = player.pSpeedActive ? &sprite.walkPSpeedTurnaround : &sprite.walkNormalTurnaround;
    } else {
        const float speed = std::fabs(player.velocity.x);
        if (speed < 0.1F) {
            clip = &sprite.idle;
        } else {
            clip = player.pSpeedActive ? &sprite.walkPSpeed : &sprite.walkNormal;
        }
    }
    if (clip != nullptr && clip->fps > 0.0F) {
        const auto& list = clip->side(right);
        if (!list.empty()) {
            frameIndex = static_cast<int>(animTime * clip->fps)
                % static_cast<int>(list.size());
        }
    }

    auto resolve = [&](const AnimClip& c) -> const Texture2D* {
        const auto& list = c.side(right);
        if (list.empty()) {
            return nullptr;
        }
        return &list[frameIndex % static_cast<int>(list.size())];
    };
    if (clip != nullptr) {
        if (const auto* t = resolve(*clip); t != nullptr) {
            out.tex = t;
            out.clip = clip;
            return out;
        }
    }
    if (const auto* t = resolve(sprite.idle); t != nullptr) {
        out.tex = t;
        out.clip = &sprite.idle;
    }
    return out;
}

const Texture2D* selectEnemyFrame(const EnemySprite& sprite,
                                  const float animTime,
                                  const float speedHint)
{
    if (!sprite.ready || sprite.frames.empty()) {
        return nullptr;
    }
    const bool moving = std::fabs(speedHint) > 0.05F;
    const float configFps = (sprite.fps > 0.0F) ? sprite.fps : 6.0F;
    const float rate = moving ? configFps : 0.0F;
    const int idx = (rate > 0.0F)
        ? static_cast<int>(animTime * rate) % static_cast<int>(sprite.frames.size())
        : 0;
    return &sprite.frames[idx];
}

} // namespace opm::client::render

#endif // OPM_CLIENT_WITH_OPENGL_STUB
