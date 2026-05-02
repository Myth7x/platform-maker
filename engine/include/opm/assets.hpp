#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace opm::assets {

struct AssetRecord {
    std::string id {};
    std::string category {};
    std::string subCategory {}; // folder name within category (e.g. "set1", "set2")
    std::filesystem::path path {};
};

struct AssetManifest {
    std::vector<AssetRecord> records {};
};

[[nodiscard]] AssetManifest buildManifest(const std::filesystem::path& root);

} // namespace opm::assets
