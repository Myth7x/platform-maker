#include "sprite_config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>

namespace opm::client::sprite {
namespace {

// =====================================================================
// Tiny JSON parser — handles just what config.json needs:
//   - object {} with string keys
//   - arrays of strings or numbers
//   - strings, numbers, booleans
//   - nested objects (for the "animations" map)
//
// Unknown keys are skipped via skipValue so the schema can grow without
// breaking older parsers. Errors include a byte offset for diagnosis.
// =====================================================================

class JsonReader {
public:
    explicit JsonReader(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] bool parseConfig(PlayerStyleConfig& out, std::string& error)
    {
        skipWs();
        if (!consume('{')) {
            error = formatErr("expected '{' at start");
            return false;
        }
        while (true) {
            skipWs();
            if (peek('}')) { consume('}'); return true; }

            std::string key;
            if (!readString(key)) {
                error = formatErr("expected string key");
                return false;
            }
            skipWs();
            if (!consume(':')) {
                error = formatErr("expected ':' after key '" + key + "'");
                return false;
            }
            skipWs();

            if (key == "animations") {
                if (!readAnimationsObject(out.animations, error)) return false;
            } else {
                // Unknown / legacy keys (e.g. an old top-level
                // height_tiles) are skipped silently. height_tiles now
                // lives inside each animation entry.
                if (!skipValue(error)) return false;
            }

            skipWs();
            if (peek(',')) { consume(','); continue; }
            if (peek('}')) { consume('}'); return true; }
            error = formatErr("expected ',' or '}' after value for '" + key + "'");
            return false;
        }
    }

    [[nodiscard]] bool parseEnemyConfig(EnemyConfig& out, std::string& error)
    {
        skipWs();
        if (!consume('{')) { error = formatErr("expected '{' at start"); return false; }
        while (true) {
            skipWs();
            if (peek('}')) { consume('}'); return true; }

            std::string key;
            if (!readString(key)) { error = formatErr("expected string key"); return false; }
            skipWs();
            if (!consume(':')) { error = formatErr("expected ':' after key '" + key + "'"); return false; }
            skipWs();

            if (key == "height_tiles") {
                double n = 0.0;
                if (!readNumber(n)) { error = formatErr("'height_tiles' must be a number"); return false; }
                out.heightTiles = static_cast<float>(n);
            } else if (key == "fps") {
                double n = 0.0;
                if (!readNumber(n)) { error = formatErr("'fps' must be a number"); return false; }
                out.fps = static_cast<float>(n);
            } else {
                if (!skipValue(error)) return false;
            }

            skipWs();
            if (peek(',')) { consume(','); continue; }
            if (peek('}')) { consume('}'); return true; }
            error = formatErr("expected ',' or '}' after value for '" + key + "'");
            return false;
        }
    }

private:
    [[nodiscard]] bool readAnimationsObject(std::map<std::string, AnimDef>& anims, std::string& error)
    {
        if (!consume('{')) { error = formatErr("expected '{' for animations"); return false; }
        while (true) {
            skipWs();
            if (peek('}')) { consume('}'); return true; }

            std::string name;
            if (!readString(name)) { error = formatErr("expected animation name"); return false; }
            skipWs();
            if (!consume(':')) { error = formatErr("expected ':' after animation name '" + name + "'"); return false; }
            skipWs();

            AnimDef def;
            if (!readAnimDef(def, error)) return false;
            anims[name] = std::move(def);

            skipWs();
            if (peek(',')) { consume(','); continue; }
            if (peek('}')) { consume('}'); return true; }
            error = formatErr("expected ',' or '}' in animations");
            return false;
        }
    }

    [[nodiscard]] bool readAnimDef(AnimDef& def, std::string& error)
    {
        if (!consume('{')) { error = formatErr("expected '{' for animation"); return false; }
        while (true) {
            skipWs();
            if (peek('}')) { consume('}'); return true; }

            std::string key;
            if (!readString(key)) { error = formatErr("expected key in animation"); return false; }
            skipWs();
            if (!consume(':')) { error = formatErr("expected ':' after '" + key + "'"); return false; }
            skipWs();

            if (key == "fps") {
                double n = 0.0;
                if (!readNumber(n)) { error = formatErr("'fps' must be a number"); return false; }
                def.fps = static_cast<float>(n);
            } else if (key == "height_tiles") {
                double n = 0.0;
                if (!readNumber(n)) { error = formatErr("'height_tiles' must be a number"); return false; }
                def.heightTiles = static_cast<float>(n);
            } else if (key == "left") {
                if (!readStringArray(def.leftPaths, error)) return false;
            } else if (key == "right") {
                if (!readStringArray(def.rightPaths, error)) return false;
            } else {
                if (!skipValue(error)) return false;
            }

            skipWs();
            if (peek(',')) { consume(','); continue; }
            if (peek('}')) { consume('}'); return true; }
            error = formatErr("expected ',' or '}' in animation");
            return false;
        }
    }

    [[nodiscard]] bool readStringArray(std::vector<std::string>& out, std::string& error)
    {
        if (!consume('[')) { error = formatErr("expected '[' for string array"); return false; }
        out.clear();
        skipWs();
        if (peek(']')) { consume(']'); return true; }
        while (true) {
            skipWs();
            std::string s;
            if (!readString(s)) { error = formatErr("expected string in array"); return false; }
            out.push_back(std::move(s));
            skipWs();
            if (peek(',')) { consume(','); continue; }
            if (peek(']')) { consume(']'); return true; }
            error = formatErr("expected ',' or ']' in string array");
            return false;
        }
    }

    // Skips any value type (string / number / bool / null / array / object)
    // including nested ones, using a brace/bracket depth counter for the
    // composite types.
    [[nodiscard]] bool skipValue(std::string& error)
    {
        skipWs();
        if (peek('"')) {
            std::string dummy;
            return readString(dummy);
        }
        if (peek('[') || peek('{')) {
            const char open = text_[pos_];
            const char close = (open == '[') ? ']' : '}';
            consume(open);
            int depth = 1;
            while (depth > 0 && pos_ < text_.size()) {
                const char c = text_[pos_++];
                if (c == '"') {
                    while (pos_ < text_.size() && text_[pos_] != '"') {
                        if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
                            pos_ += 2;
                        } else {
                            ++pos_;
                        }
                    }
                    if (pos_ < text_.size()) ++pos_;
                } else if (c == open) {
                    ++depth;
                } else if (c == close) {
                    --depth;
                }
            }
            if (depth != 0) {
                error = formatErr("unterminated array/object");
                return false;
            }
            return true;
        }
        // number / true / false / null — scan until terminator.
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ',' || c == '}' || c == ']' ||
                std::isspace(static_cast<unsigned char>(c))) break;
            ++pos_;
        }
        return true;
    }

    void skipWs() noexcept
    {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char c) noexcept
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
        if (!consume('"')) return false;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c == '\\' && pos_ < text_.size()) {
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    default:   out.push_back(esc);  break;
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
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            const bool numChar = (c >= '0' && c <= '9') || c == '.'
                || c == 'e' || c == 'E' || c == '+' || c == '-';
            if (!numChar) break;
            ++pos_;
        }
        if (pos_ == start) return false;
        const std::string token(text_.substr(start, pos_ - start));
        out = std::strtod(token.c_str(), nullptr);
        return true;
    }

    std::string formatErr(const std::string& msg) const
    {
        std::ostringstream oss;
        oss << msg << " (at byte " << pos_ << ")";
        return oss.str();
    }

    std::string_view text_;
    std::size_t pos_ {0};
};

} // namespace

namespace {

bool readConfigFile(const std::filesystem::path& path, std::string& text)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    text = contents.str();
    return true;
}

} // namespace

bool loadPlayerStyleConfig(const std::filesystem::path& styleDir,
                           PlayerStyleConfig& out,
                           std::string& error)
{
    out = PlayerStyleConfig {};
    error.clear();

    std::string text;
    if (!readConfigFile(styleDir / "config.json", text) || text.empty()) {
        // Missing or empty stub — caller falls back to convention scanning.
        return true;
    }

    JsonReader reader(text);
    if (!reader.parseConfig(out, error)) {
        return false;
    }
    return true;
}

bool loadEnemyConfig(const std::filesystem::path& enemyDir,
                     EnemyConfig& out,
                     std::string& error)
{
    out = EnemyConfig {};
    error.clear();

    std::string text;
    if (!readConfigFile(enemyDir / "config.json", text) || text.empty()) {
        return true;
    }

    JsonReader reader(text);
    if (!reader.parseEnemyConfig(out, error)) {
        return false;
    }
    return true;
}

} // namespace opm::client::sprite
