#include "opm/assets.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>

namespace opm::assets {
namespace {

std::string normalize(const std::string& text)
{
    std::string out;
    out.reserve(text.size());

    for (const char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }

    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }

    if (out.empty()) {
        out = "asset";
    }

    return out;
}

// ---------------------------------------------------------------------------
// Flat category (BG, Object): recursive walk, normalized stem IDs.
// ---------------------------------------------------------------------------
void addFlatCategory(AssetManifest& manifest, const std::filesystem::path& root, const std::string& category)
{
    const auto dir = root / category;
    if (!std::filesystem::exists(dir)) {
        return;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".png") {
            continue;
        }
        AssetRecord record;
        record.category = category;
        record.path = entry.path();
        record.id = category + ":" + normalize(entry.path().stem().string());
        manifest.records.push_back(record);
    }
}

// ---------------------------------------------------------------------------
// Tile set helpers: parse "tile_ROW_COL" or plain integer stems for sort order.
// ---------------------------------------------------------------------------

struct TileFile {
    std::filesystem::path path;
    std::string subDir;
    std::string stem;
    bool isRowCol {false};
    int row {0};
    int col {0};
    bool isNumeric {false};
    int num {0};
};

[[nodiscard]] bool parseTileRowCol(const std::string& stem, int& row, int& col)
{
    constexpr std::string_view kPrefix = "tile_";
    if (stem.size() <= kPrefix.size() || stem.substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }
    const auto rest = stem.substr(kPrefix.size());
    const auto sep  = rest.find('_');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= rest.size()) {
        return false;
    }
    const auto rowStr = rest.substr(0, sep);
    const auto colStr = rest.substr(sep + 1);

    int r = 0;
    int c = 0;
    const auto [p1, ec1] = std::from_chars(rowStr.data(), rowStr.data() + rowStr.size(), r);
    if (ec1 != std::errc {} || p1 != rowStr.data() + rowStr.size()) {
        return false;
    }
    const auto [p2, ec2] = std::from_chars(colStr.data(), colStr.data() + colStr.size(), c);
    if (ec2 != std::errc {} || p2 != colStr.data() + colStr.size()) {
        return false;
    }

    row = r;
    col = c;
    return true;
}

[[nodiscard]] bool parsePlainInt(const std::string& stem, int& num)
{
    if (stem.empty()) {
        return false;
    }
    int v = 0;
    const auto [p, ec] = std::from_chars(stem.data(), stem.data() + stem.size(), v);
    if (ec != std::errc {} || p != stem.data() + stem.size()) {
        return false;
    }
    num = v;
    return true;
}

// ---------------------------------------------------------------------------
// Tiles category: multi-set with structured naming.
//
// Each subfolder of Tiles/ is a "set" (e.g. set1, set2, ...).
// Files inside a set are sorted:
//   - "tile_R_C.png"  -> sorted by (R, C) numerically (row-major)
//   - "N.png"         -> sorted by N numerically
//   - anything else   -> alphabetically (fallback)
//
// A single sequential uint16 index (starting at 1) is assigned globally
// across all sets in alphabetical folder order. The resulting IDs are
// "Tiles:1", "Tiles:2", ... which the palette and tile-layer code consume
// directly. The subCategory field holds the set folder name for UI grouping.
// ---------------------------------------------------------------------------
void addTilesCategory(AssetManifest& manifest, const std::filesystem::path& root)
{
    const auto tilesDir = root / "Tiles";
    if (!std::filesystem::exists(tilesDir)) {
        return;
    }

    // Collect sorted subdirectory names.
    std::vector<std::string> subDirs;
    for (const auto& entry : std::filesystem::directory_iterator(tilesDir)) {
        if (entry.is_directory()) {
            subDirs.push_back(entry.path().filename().string());
        }
    }
    std::sort(subDirs.begin(), subDirs.end());

    // Collect all tile files across all sets.
    std::vector<TileFile> tileFiles;
    for (const auto& subDir : subDirs) {
        const auto subPath = tilesDir / subDir;
        for (const auto& entry : std::filesystem::directory_iterator(subPath)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".png") {
                continue;
            }
            TileFile tf;
            tf.path    = entry.path();
            tf.subDir  = subDir;
            tf.stem    = entry.path().stem().string();
            if (parseTileRowCol(tf.stem, tf.row, tf.col)) {
                tf.isRowCol = true;
            } else if (parsePlainInt(tf.stem, tf.num)) {
                tf.isNumeric = true;
            }
            tileFiles.push_back(std::move(tf));
        }
    }

    // Sort: subDir alphabetically, then row/col or num numerically.
    std::sort(tileFiles.begin(), tileFiles.end(),
        [](const TileFile& a, const TileFile& b) {
            if (a.subDir != b.subDir) {
                return a.subDir < b.subDir;
            }
            if (a.isRowCol && b.isRowCol) {
                if (a.row != b.row) { return a.row < b.row; }
                return a.col < b.col;
            }
            if (a.isNumeric && b.isNumeric) {
                return a.num < b.num;
            }
            // row-col tiles sort before numeric tiles, both before fallback.
            if (a.isRowCol != b.isRowCol) { return static_cast<int>(a.isRowCol) > static_cast<int>(b.isRowCol); }
            if (a.isNumeric != b.isNumeric) { return static_cast<int>(a.isNumeric) > static_cast<int>(b.isNumeric); }
            return a.stem < b.stem;
        });

    // Assign sequential uint16 indices starting from 1.
    std::uint16_t index = 1;
    for (const auto& tf : tileFiles) {
        AssetRecord record;
        record.category    = "Tiles";
        record.subCategory = tf.subDir;
        record.path        = tf.path;
        record.id          = "Tiles:" + std::to_string(index);
        manifest.records.push_back(record);
        ++index;
    }
}

} // namespace

AssetManifest buildManifest(const std::filesystem::path& root)
{
    AssetManifest manifest;

    addFlatCategory(manifest, root, "BG");
    addTilesCategory(manifest, root);    // tiles keep their insertion order (already sorted)
    addFlatCategory(manifest, root, "Object");

    // Sort BG and Object records by id; preserve Tiles insertion order.
    std::stable_sort(manifest.records.begin(), manifest.records.end(),
        [](const AssetRecord& lhs, const AssetRecord& rhs) {
            if (lhs.category != rhs.category) {
                return lhs.category < rhs.category;
            }
            if (lhs.category == "Tiles") {
                return false; // preserve insertion order
            }
            return lhs.id < rhs.id;
        });

    return manifest;
}

} // namespace opm::assets
