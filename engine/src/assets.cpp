#include "opm/assets.hpp"

#include <algorithm>
#include <cctype>
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

void addCategory(AssetManifest& manifest, const std::filesystem::path& root, const std::string& category)
{
    const auto dir = root / category;
    if (!std::filesystem::exists(dir)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".png") {
            continue;
        }

        AssetRecord record;
        record.category = category;
        record.path = entry.path();
        record.id = category + ":" + normalize(entry.path().stem().string());
        manifest.records.push_back(record);
    }
}

} // namespace

AssetManifest buildManifest(const std::filesystem::path& root)
{
    AssetManifest manifest;
    addCategory(manifest, root, "BG");
    addCategory(manifest, root, "Tiles");
    addCategory(manifest, root, "Object");

    std::ranges::sort(manifest.records, [](const AssetRecord& lhs, const AssetRecord& rhs) {
        if (lhs.category == rhs.category) {
            return lhs.id < rhs.id;
        }
        return lhs.category < rhs.category;
    });

    return manifest;
}

} // namespace opm::assets
