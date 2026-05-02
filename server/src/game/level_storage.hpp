#pragma once
#include "opm/level.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace opm::server {

// Loads/saves levels as JSON files under a single directory. Level names are
// taken from filenames without the ".json" extension. The directory is created
// on first save if it does not exist.
class LevelStorage {
public:
    explicit LevelStorage(std::filesystem::path directory);

    // Lazily refreshes the in-memory index from disk and returns the names.
    [[nodiscard]] std::vector<std::string> listNames();

    // Loads the named level. Returns true on success; sets `error` on failure.
    [[nodiscard]] bool load(std::string_view name, opm::engine::LevelData& out, std::string& error) const;

    // Persists the named level to disk as `<name>.json`. Returns true on success;
    // sets `error` on failure. Rejects names with path-traversal or unsupported
    // characters.
    [[nodiscard]] bool save(std::string_view name, const opm::engine::LevelData& level, std::string& error);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

private:
    [[nodiscard]] static bool isSafeName(std::string_view name) noexcept;

    std::filesystem::path directory_;
};

} // namespace opm::server
