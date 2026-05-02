#include "opm/level_json.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>

namespace opm::engine {
namespace {

// =====================================================================
// Tiny purpose-specific JSON parser. Handles a single object with string
// keys whose values are numbers or arrays of numbers — the schema described
// in level_json.hpp. No support for nested objects, booleans, or null.
// =====================================================================

class JsonReader {
public:
    explicit JsonReader(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] bool parseObject(LevelData& out, std::string& error)
    {
        skipWhitespace();
        if (!consume('{')) {
            error = "expected '{' at start";
            return false;
        }
        skipWhitespace();
        if (peek('}')) {
            (void)consume('}');
            return true;
        }

        while (true) {
            skipWhitespace();
            std::string key;
            if (!readString(key)) {
                error = "expected string key";
                return false;
            }
            skipWhitespace();
            if (!consume(':')) {
                error = "expected ':' after key '" + key + "'";
                return false;
            }
            skipWhitespace();
            if (!dispatchValue(key, out, error)) {
                return false;
            }
            skipWhitespace();
            if (peek(',')) {
                (void)consume(',');
                continue;
            }
            if (peek('}')) {
                (void)consume('}');
                return true;
            }
            error = "expected ',' or '}' after value for '" + key + "'";
            return false;
        }
    }

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    [[nodiscard]] bool dispatchValue(const std::string& key, LevelData& out, std::string& error)
    {
        if (key == "width") {
            std::uint64_t v = 0;
            if (!readUInt(v)) { error = "width must be integer"; return false; }
            width_ = static_cast<std::uint32_t>(v);
            return true;
        }
        if (key == "height") {
            std::uint64_t v = 0;
            if (!readUInt(v)) { error = "height must be integer"; return false; }
            height_ = static_cast<std::uint32_t>(v);
            return true;
        }
        if (key == "spawnX") { return readFloat(out.spawnX, error, "spawnX"); }
        if (key == "spawnY") { return readFloat(out.spawnY, error, "spawnY"); }
        if (key == "goalX")  { return readFloat(out.goalX,  error, "goalX"); }
        if (key == "goalY")  { return readFloat(out.goalY,  error, "goalY"); }
        if (key == "background") {
            return readU16Array(out.background.tileIndices, error);
        }
        if (key == "foliage") {
            return readU16Array(out.foliage.tileIndices, error);
        }
        if (key == "foreground") {
            return readU16Array(out.foreground.tileIndices, error);
        }
        if (key == "tiles") {
            // Backward compatibility with single-layer format: load into foliage.
            return readU16Array(out.foliage.tileIndices, error);
        }
        if (key == "actors") {
            return readActorArray(out.actors, error);
        }
        if (key == "tileOverrides") {
            return readTileOverrides(out.tileCollisionOverrides, error);
        }
        // Unknown key — skip its value gracefully.
        return skipValue(error);
    }

    [[nodiscard]] bool readTileOverrides(std::map<std::uint16_t, TileCollisionMask>& out,
                                         std::string& error)
    {
        // Object keyed by stringified tile id, value is an object of
        // {top, bottom, left, right, oneWayTop} flags (each 0 or 1,
        // missing = 0).
        if (!consume('{')) { error = "expected '{' for tileOverrides"; return false; }
        out.clear();
        skipWhitespace();
        if (peek('}')) { (void)consume('}'); return true; }
        while (true) {
            skipWhitespace();
            std::string key;
            if (!readString(key)) { error = "expected tile-id key in tileOverrides"; return false; }
            std::uint64_t tileId = 0;
            const auto* begin = key.data();
            const auto* end = key.data() + key.size();
            if (auto [ptr, ec] = std::from_chars(begin, end, tileId);
                ec != std::errc {} || ptr != end) {
                error = "tileOverrides key must be an integer string";
                return false;
            }
            skipWhitespace();
            if (!consume(':')) { error = "expected ':' after tileOverrides key"; return false; }
            skipWhitespace();
            if (!consume('{')) { error = "expected '{' for tileOverride entry"; return false; }
            TileCollisionMask mask {};
            while (true) {
                skipWhitespace();
                if (peek('}')) { (void)consume('}'); break; }
                std::string flagKey;
                if (!readString(flagKey)) { error = "expected flag name in tileOverride"; return false; }
                skipWhitespace();
                if (!consume(':')) { error = "expected ':' after flag name"; return false; }
                skipWhitespace();
                std::uint64_t v = 0;
                if (!readUInt(v)) { error = "tileOverride flag value must be integer"; return false; }
                if      (flagKey == "top")       mask.solidTop    = (v != 0U);
                else if (flagKey == "bottom")    mask.solidBottom = (v != 0U);
                else if (flagKey == "left")      mask.solidLeft   = (v != 0U);
                else if (flagKey == "right")     mask.solidRight  = (v != 0U);
                else if (flagKey == "oneWayTop") mask.oneWayTop   = (v != 0U);
                // Unknown flag keys are silently ignored.
                skipWhitespace();
                if (peek(',')) { (void)consume(','); continue; }
                if (peek('}')) { (void)consume('}'); break; }
                error = "expected ',' or '}' in tileOverride entry";
                return false;
            }
            out[static_cast<std::uint16_t>(tileId)] = mask;
            skipWhitespace();
            if (peek(',')) { (void)consume(','); continue; }
            if (peek('}')) { (void)consume('}'); return true; }
            error = "expected ',' or '}' in tileOverrides";
            return false;
        }
    }

    [[nodiscard]] bool readActorArray(std::vector<ActorSpawn>& out, std::string& error)
    {
        if (!consume('[')) { error = "expected '[' for actors array"; return false; }
        out.clear();
        skipWhitespace();
        if (peek(']')) { (void)consume(']'); return true; }

        while (true) {
            skipWhitespace();
            if (!consume('{')) { error = "expected '{' in actors array"; return false; }
            ActorSpawn a {};
            while (true) {
                skipWhitespace();
                if (peek('}')) { (void)consume('}'); break; }
                std::string key;
                if (!readString(key)) { error = "expected actor field key"; return false; }
                skipWhitespace();
                if (!consume(':')) { error = "expected ':' in actor"; return false; }
                skipWhitespace();
                if (key == "x") {
                    double v = 0; if (!readNumber(v)) { error = "actor.x must be number"; return false; }
                    a.x = static_cast<float>(v);
                } else if (key == "y") {
                    double v = 0; if (!readNumber(v)) { error = "actor.y must be number"; return false; }
                    a.y = static_cast<float>(v);
                } else if (key == "script") {
                    std::uint64_t v = 0; if (!readUInt(v)) { error = "actor.script must be integer"; return false; }
                    a.script = static_cast<ActorScript>(v & 0xFFU);
                } else if (key == "diesWhenStomped") {
                    std::uint64_t v = 0; if (!readUInt(v)) { error = "actor.diesWhenStomped must be 0/1"; return false; }
                    a.diesWhenStomped = (v != 0U);
                } else if (key == "canJumpObstacles") {
                    std::uint64_t v = 0; if (!readUInt(v)) { error = "actor.canJumpObstacles must be 0/1"; return false; }
                    a.canJumpObstacles = (v != 0U);
                } else if (key == "canJumpRandom") {
                    std::uint64_t v = 0; if (!readUInt(v)) { error = "actor.canJumpRandom must be 0/1"; return false; }
                    a.canJumpRandom = (v != 0U);
                } else if (key == "canFly") {
                    std::uint64_t v = 0; if (!readUInt(v)) { error = "actor.canFly must be 0/1"; return false; }
                    a.canFly = (v != 0U);
                } else if (key == "kind") {
                    std::uint64_t v = 0; if (!readUInt(v)) { error = "actor.kind must be integer"; return false; }
                    a.enemyKind = static_cast<std::uint8_t>(v & 0xFFU);
                } else if (key == "category") {
                    std::uint64_t v = 0; if (!readUInt(v)) { error = "actor.category must be integer"; return false; }
                    a.category = static_cast<ActorCategory>(v & 0xFFU);
                } else {
                    if (!skipValue(error)) return false;
                }
                skipWhitespace();
                if (peek(',')) { (void)consume(','); continue; }
                if (peek('}')) { (void)consume('}'); break; }
                error = "expected ',' or '}' in actor";
                return false;
            }
            out.push_back(a);
            skipWhitespace();
            if (peek(',')) { (void)consume(','); continue; }
            if (peek(']')) { (void)consume(']'); return true; }
            error = "expected ',' or ']' in actors array";
            return false;
        }
    }

    [[nodiscard]] bool readFloat(float& out, std::string& error, const char* field)
    {
        double v = 0.0;
        if (!readNumber(v)) {
            error = std::string(field) + " must be number";
            return false;
        }
        out = static_cast<float>(v);
        return true;
    }

    [[nodiscard]] bool readU16Array(std::vector<std::uint16_t>& out, std::string& error)
    {
        if (!consume('[')) {
            error = "expected '[' for tiles array";
            return false;
        }
        out.clear();
        skipWhitespace();
        if (peek(']')) { (void)consume(']'); return true; }

        while (true) {
            skipWhitespace();
            std::uint64_t v = 0;
            if (!readUInt(v)) {
                error = "tiles array must contain integers";
                return false;
            }
            out.push_back(static_cast<std::uint16_t>(v));
            skipWhitespace();
            if (peek(',')) { (void)consume(','); continue; }
            if (peek(']')) { (void)consume(']'); return true; }
            error = "expected ',' or ']' in tiles array";
            return false;
        }
    }

    [[nodiscard]] bool skipValue(std::string& error)
    {
        skipWhitespace();
        if (peek('"')) {
            std::string dummy;
            return readString(dummy);
        }
        if (peek('[') || peek('{')) {
            const char open = text_[pos_];
            const char close = (open == '[') ? ']' : '}';
            (void)consume(open);
            int depth = 1;
            while (depth > 0 && pos_ < text_.size()) {
                const char c = text_[pos_++];
                if (c == '"') { while (pos_ < text_.size() && text_[pos_] != '"') ++pos_; if (pos_ < text_.size()) ++pos_; }
                else if (c == open) { ++depth; }
                else if (c == close) { --depth; }
            }
            if (depth != 0) { error = "unterminated array/object"; return false; }
            return true;
        }
        // Scan number, true, false, null.
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ',' || c == '}' || c == ']' || std::isspace(static_cast<unsigned char>(c))) break;
            ++pos_;
        }
        return true;
    }

    void skipWhitespace() noexcept
    {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    [[nodiscard]] bool consume(char c) noexcept
    {
        if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    [[nodiscard]] bool peek(char c) const noexcept
    {
        return pos_ < text_.size() && text_[pos_] == c;
    }

    [[nodiscard]] bool readString(std::string& out)
    {
        if (!consume('"')) {
            return false;
        }
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\' && pos_ < text_.size()) {
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    default: out.push_back(esc); break;
                }
                continue;
            }
            out.push_back(c);
        }
        return false; // unterminated
    }

    [[nodiscard]] bool readNumber(double& out) noexcept
    {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            ++pos_;
        }
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            const bool numChar = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
            if (!numChar) break;
            ++pos_;
        }
        if (pos_ == start) {
            return false;
        }
        const std::string token(text_.substr(start, pos_ - start));
        out = std::strtod(token.c_str(), nullptr);
        return true;
    }

    [[nodiscard]] bool readUInt(std::uint64_t& out) noexcept
    {
        skipWhitespace();
        const std::size_t start = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
        if (pos_ == start) {
            return false;
        }
        const auto* begin = text_.data() + start;
        const auto* end = text_.data() + pos_;
        auto [ptr, ec] = std::from_chars(begin, end, out);
        return ec == std::errc {} && ptr == end;
    }

    std::string_view text_;
    std::size_t pos_ {0};
    std::uint32_t width_ {0};
    std::uint32_t height_ {0};
};

void appendFloat(std::string& out, float value)
{
    std::array<char, 64> buf {};
    auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    if (ec == std::errc {}) {
        out.append(buf.data(), ptr);
    } else {
        out += "0";
    }
}

} // namespace

namespace {

void appendTileArray(std::string& out, const TileLayer& layer, const char* fieldName)
{
    out += "  \"";
    out += fieldName;
    out += "\": [";
    for (std::size_t i = 0; i < layer.tileIndices.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += std::to_string(layer.tileIndices[i]);
    }
    out += "]";
}

} // namespace

std::string levelToJson(const LevelData& level)
{
    std::string out;
    const std::size_t totalTiles = level.background.tileIndices.size()
        + level.foliage.tileIndices.size() + level.foreground.tileIndices.size();
    out.reserve(384 + totalTiles * 5);
    out += "{\n";
    out += "  \"width\": " + std::to_string(level.foliage.width) + ",\n";
    out += "  \"height\": " + std::to_string(level.foliage.height) + ",\n";
    out += "  \"spawnX\": "; appendFloat(out, level.spawnX); out += ",\n";
    out += "  \"spawnY\": "; appendFloat(out, level.spawnY); out += ",\n";
    out += "  \"goalX\": ";  appendFloat(out, level.goalX);  out += ",\n";
    out += "  \"goalY\": ";  appendFloat(out, level.goalY);  out += ",\n";
    appendTileArray(out, level.background, "background"); out += ",\n";
    appendTileArray(out, level.foliage,    "foliage");    out += ",\n";
    appendTileArray(out, level.foreground, "foreground"); out += ",\n";

    out += "  \"actors\": [";
    for (std::size_t i = 0; i < level.actors.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        const auto& a = level.actors[i];
        out += "{\"x\":";
        appendFloat(out, a.x);
        out += ",\"y\":";
        appendFloat(out, a.y);
        out += ",\"script\":";
        out += std::to_string(static_cast<unsigned>(a.script));
        if (a.diesWhenStomped) out += ",\"diesWhenStomped\":1";
        if (a.canJumpObstacles) out += ",\"canJumpObstacles\":1";
        if (a.canJumpRandom) out += ",\"canJumpRandom\":1";
        if (a.canFly) out += ",\"canFly\":1";
        if (a.enemyKind != 0U) {
            out += ",\"kind\":";
            out += std::to_string(static_cast<unsigned>(a.enemyKind));
        }
        if (a.category != ActorCategory::Enemy) {
            out += ",\"category\":";
            out += std::to_string(static_cast<unsigned>(a.category));
        }
        out += '}';
    }
    out += "]";

    if (!level.tileCollisionOverrides.empty()) {
        out += ",\n  \"tileOverrides\": {";
        bool first = true;
        for (const auto& [id, mask] : level.tileCollisionOverrides) {
            if (!first) {
                out += ',';
            }
            first = false;
            out += '"';
            out += std::to_string(static_cast<unsigned>(id));
            out += "\":{";
            bool firstFlag = true;
            const auto appendFlag = [&](const char* name, bool v) {
                if (!v) return;
                if (!firstFlag) out += ',';
                firstFlag = false;
                out += '"';
                out += name;
                out += "\":1";
            };
            appendFlag("top",       mask.solidTop);
            appendFlag("bottom",    mask.solidBottom);
            appendFlag("left",      mask.solidLeft);
            appendFlag("right",     mask.solidRight);
            appendFlag("oneWayTop", mask.oneWayTop);
            out += '}';
        }
        out += "}";
    }
    out += "\n}\n";
    return out;
}

bool parseLevelFromJson(std::string_view text, LevelData& out, std::string& error)
{
    out = {};
    JsonReader reader(text);
    if (!reader.parseObject(out, error)) {
        return false;
    }
    const std::uint32_t width = reader.width();
    const std::uint32_t height = reader.height();
    if (width == 0U || height == 0U) {
        error = "width and height must be non-zero";
        return false;
    }
    if (width > kMaxLevelDimension || height > kMaxLevelDimension) {
        error = "level exceeds max dimension " + std::to_string(kMaxLevelDimension);
        return false;
    }
    const auto expectedTiles = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    auto fillLayer = [&](TileLayer& layer, const char* name) -> bool {
        layer.width = width;
        layer.height = height;
        if (layer.tileIndices.empty()) {
            // Layer omitted → treat as empty.
            layer.tileIndices.assign(expectedTiles, 0U);
            return true;
        }
        if (layer.tileIndices.size() != expectedTiles) {
            error = std::string(name) + " tile count mismatch: expected " + std::to_string(expectedTiles)
                  + " got " + std::to_string(layer.tileIndices.size());
            return false;
        }
        return true;
    };
    if (!fillLayer(out.background, "background")) return false;
    if (!fillLayer(out.foliage,    "foliage"))    return false;
    if (!fillLayer(out.foreground, "foreground")) return false;
    return true;
}

} // namespace opm::engine
