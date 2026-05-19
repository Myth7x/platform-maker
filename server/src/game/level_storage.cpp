#include "game/level_storage.hpp"

#include "opm/level_json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace opm::server {

LevelStorage::LevelStorage(std::filesystem::path directory)
    : directory_(std::move(directory))
{
}

bool LevelStorage::isSafeName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        const bool isAlpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool isDigit = (c >= '0' && c <= '9');
        const bool isSep = (c == '_' || c == '-');
        if (!isAlpha && !isDigit && !isSep) {
            return false;
        }
    }
    return true;
}

const std::vector<std::string>& LevelStorage::listNames()
{
    if (!namesCacheValid_) {
        refreshNameCache();
    }
    return cachedNames_;
}

void LevelStorage::refreshNameCache()
{
    cachedNames_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(directory_, ec)) {
        namesCacheValid_ = true;
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto& path = entry.path();
        if (path.extension() != ".json") {
            continue;
        }
        const auto stem = path.stem().string();
        if (!isSafeName(stem)) {
            continue;
        }
        cachedNames_.push_back(stem);
    }
    std::sort(cachedNames_.begin(), cachedNames_.end());
    namesCacheValid_ = true;
}

bool LevelStorage::load(std::string_view name, opm::engine::LevelData& out, std::string& error) const
{
    if (!isSafeName(name)) {
        error = "invalid level name";
        return false;
    }
    const auto path = directory_ / (std::string(name) + ".json");
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        error = "level not found";
        return false;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string text = contents.str();
    if (!opm::engine::parseLevelFromJson(text, out, error)) {
        return false;
    }
    return true;
}

bool LevelStorage::save(std::string_view name, const opm::engine::LevelData& level, std::string& error)
{
    if (!isSafeName(name)) {
        error = "invalid level name (use letters, digits, underscore or dash; 1-64 chars)";
        return false;
    }
    const auto width = level.foliage.width;
    const auto height = level.foliage.height;
    if (width == 0U || height == 0U) {
        error = "level dimensions must be non-zero";
        return false;
    }
    if (width > opm::engine::kMaxLevelDimension || height > opm::engine::kMaxLevelDimension) {
        error = "level exceeds max dimension " + std::to_string(opm::engine::kMaxLevelDimension);
        return false;
    }
    const auto expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    auto checkLayer = [&](const opm::engine::TileLayer& layer, const char* layerName) -> bool {
        if (layer.width != width || layer.height != height || layer.tileIndices.size() != expected) {
            error = std::string(layerName) + " tile dimensions do not match width*height";
            return false;
        }
        return true;
    };
    if (!checkLayer(level.background, "background")) return false;
    if (!checkLayer(level.foliage,    "foliage"))    return false;
    if (!checkLayer(level.foreground, "foreground")) return false;

    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        error = "could not create levels directory: " + ec.message();
        return false;
    }

    const auto path = directory_ / (std::string(name) + ".json");
    const std::string text = opm::engine::levelToJson(level);

    // Atomic-ish: write to temp file, then rename.
    const auto tempPath = path.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            error = "could not open level file for writing";
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            error = "write failed";
            return false;
        }
    }
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        // Fallback: copy + remove.
        std::filesystem::copy_file(tempPath, path,
            std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tempPath);
        if (ec) {
            error = "rename failed: " + ec.message();
            return false;
        }
    }
    // A new file may have been created — invalidate the name cache.
    namesCacheValid_ = false;
    return true;
}

} // namespace opm::server
