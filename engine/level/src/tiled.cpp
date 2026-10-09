#include "forge/level/tiled.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/path.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <memory>

namespace forge::level::tiled {

namespace fs = std::filesystem;

namespace {

// --- a small XML reader: elements, attributes, text; what Tiled writes ---

struct Node {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Node> children;
    std::string text;
    u32 line = 0;
    const std::string* attr(std::string_view n) const {
        for (const auto& [k, v] : attrs)
            if (k == n) return &v;
        return nullptr;
    }
    std::string str(std::string_view n, std::string_view fallback = {}) const {
        const std::string* v = attr(n);
        return v ? *v : std::string(fallback);
    }
};

class XmlReader {
public:
    explicit XmlReader(std::string_view text) : s_(text) {}

    bool read(Node& root, std::string& error) {
        skip_prolog();
        if (!error_.empty() || at_end() || s_[i_] != '<') return fail(error, "это не XML: в начале нет элемента");
        if (!element(root, 0)) return fail(error, error_);
        skip_misc();
        if (!error_.empty()) return fail(error, error_);
        if (!at_end()) return fail(error, "после корневого элемента что-то ещё (строка " + std::to_string(line_) + ")");
        return true;
    }

private:
    bool fail(std::string& error, const std::string& why) {
        error = why.empty() ? "это не XML" : why;
        return false;
    }
    bool at_end() const { return i_ >= s_.size(); }
    bool starts(std::string_view p) const { return s_.substr(i_, p.size()) == p; }
    void advance(usize n) {
        for (usize k = 0; k < n && i_ < s_.size(); ++k, ++i_)
            if (s_[i_] == '\n') ++line_;
    }
    void skip_space() {
        while (!at_end() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) advance(1);
    }
    bool skip_until(std::string_view end) {
        const usize at = s_.find(end, i_);
        if (at == std::string_view::npos) {
            error_ = "XML оборван (строка " + std::to_string(line_) + ")";
            i_ = s_.size();
            return false;
        }
        advance(at + end.size() - i_);
        return true;
    }
    // Whitespace, comments, processing instructions, a DOCTYPE.
    void skip_misc() {
        for (;;) {
            skip_space();
            if (starts("<!--")) {
                if (!skip_until("-->")) return;
            } else if (starts("<?")) {
                if (!skip_until("?>")) return;
            } else if (starts("<!DOCTYPE")) {
                if (!skip_until(">")) return;
            } else {
                return;
            }
        }
    }
    void skip_prolog() {
        if (starts("\xEF\xBB\xBF")) advance(3); // a UTF-8 byte order mark
        skip_misc();
    }
    static bool name_char(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':' ||
               static_cast<unsigned char>(c) >= 0x80;
    }
    std::string name() {
        const usize from = i_;
        while (!at_end() && name_char(s_[i_])) advance(1);
        return std::string(s_.substr(from, i_ - from));
    }
    // Text with its entities resolved.
    bool unescape(std::string_view raw, std::string& out) {
        out.clear();
        out.reserve(raw.size());
        for (usize k = 0; k < raw.size(); ++k) {
            if (raw[k] != '&') {
                out += raw[k];
                continue;
            }
            const usize semi = raw.find(';', k);
            if (semi == std::string_view::npos) {
                error_ = "в тексте & без ; (строка " + std::to_string(line_) + ")";
                return false;
            }
            const std::string_view e = raw.substr(k + 1, semi - k - 1);
            if (e == "lt") out += '<';
            else if (e == "gt") out += '>';
            else if (e == "amp") out += '&';
            else if (e == "quot") out += '"';
            else if (e == "apos") out += '\'';
            else if (e.size() > 1 && e[0] == '#') {
                u32 code = 0;
                const bool hex = e[1] == 'x' || e[1] == 'X';
                const std::string_view digits = e.substr(hex ? 2 : 1);
                const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
                if (r.ec != std::errc() || r.ptr != digits.data() + digits.size() || code == 0 || code > 0x10FFFF) {
                    error_ = "неверный символ &" + std::string(e) + "; (строка " + std::to_string(line_) + ")";
                    return false;
                }
                // UTF-8.
                if (code < 0x80) out += static_cast<char>(code);
                else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else if (code < 0x10000) {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (code >> 18));
                    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
            } else {
                error_ = "неизвестное имя &" + std::string(e) + "; (строка " + std::to_string(line_) + ")";
                return false;
            }
            k = semi;
        }
        return true;
    }
    bool element(Node& n, u32 depth) {
        if (depth > 64) {
            error_ = "слишком глубокая вложенность элементов";
            return false;
        }
        n.line = line_;
        advance(1); // <
        n.name = name();
        if (n.name.empty()) {
            error_ = "у элемента нет имени (строка " + std::to_string(line_) + ")";
            return false;
        }
        // Attributes.
        for (;;) {
            skip_space();
            if (at_end()) {
                error_ = "XML оборван в элементе <" + n.name + ">";
                return false;
            }
            if (starts("/>")) {
                advance(2);
                return true;
            }
            if (s_[i_] == '>') {
                advance(1);
                break;
            }
            std::string key = name();
            skip_space();
            if (key.empty() || at_end() || s_[i_] != '=') {
                error_ = "неверный атрибут в <" + n.name + "> (строка " + std::to_string(line_) + ")";
                return false;
            }
            advance(1);
            skip_space();
            if (at_end() || (s_[i_] != '"' && s_[i_] != '\'')) {
                error_ = "значение атрибута " + key + " без кавычек (строка " + std::to_string(line_) + ")";
                return false;
            }
            const char quote = s_[i_];
            advance(1);
            const usize end = s_.find(quote, i_);
            if (end == std::string_view::npos) {
                error_ = "XML оборван в атрибуте " + key;
                return false;
            }
            std::string value;
            if (!unescape(s_.substr(i_, end - i_), value)) return false;
            advance(end + 1 - i_);
            n.attrs.emplace_back(std::move(key), std::move(value));
        }
        // Content.
        for (;;) {
            const usize lt = s_.find('<', i_);
            if (lt == std::string_view::npos) {
                error_ = "у <" + n.name + "> нет закрывающего тега";
                return false;
            }
            if (lt > i_) {
                std::string text;
                if (!unescape(s_.substr(i_, lt - i_), text)) return false;
                n.text += text;
                advance(lt - i_);
            }
            if (starts("</")) {
                advance(2);
                const std::string closing = name();
                skip_space();
                if (closing != n.name || at_end() || s_[i_] != '>') {
                    error_ = "<" + n.name + "> закрыт как </" + closing + "> (строка " + std::to_string(line_) + ")";
                    return false;
                }
                advance(1);
                return true;
            }
            if (starts("<!--")) {
                if (!skip_until("-->")) return false;
            } else if (starts("<![CDATA[")) {
                advance(9);
                const usize end = s_.find("]]>", i_);
                if (end == std::string_view::npos) {
                    error_ = "XML оборван в CDATA";
                    return false;
                }
                n.text += std::string(s_.substr(i_, end - i_));
                advance(end + 3 - i_);
            } else if (starts("<?")) {
                if (!skip_until("?>")) return false;
            } else {
                n.children.emplace_back();
                if (!element(n.children.back(), depth + 1)) return false;
            }
        }
    }

    std::string_view s_;
    usize i_ = 0;
    u32 line_ = 1;
    std::string error_;
};

bool read_xml(const fs::path& file, Node& root, std::string& error) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) {
        error = "файл не читается: " + path_to_utf8(file);
        return false;
    }
    XmlReader reader({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    if (!reader.read(root, error)) {
        error = path_to_utf8(file.filename()) + ": " + error;
        return false;
    }
    return true;
}

// --- numbers ---

bool to_i64(std::string_view s, i64& out) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return !s.empty() && r.ec == std::errc() && r.ptr == s.data() + s.size();
}
bool to_f64(std::string_view s, f64& out) {
    std::string t(s);
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return !t.empty() && end == t.c_str() + t.size() && std::isfinite(out);
}

// Reads the attributes it is given into numbers; the first that is not one is the error.
struct Attrs {
    const Node& n;
    std::string& error;
    std::string where;
    bool u(std::string_view key, u32& out) {
        const std::string* v = n.attr(key);
        if (!v) return true;
        i64 x = 0;
        if (!to_i64(*v, x) || x < 0 || x > 0xFFFFFFFFll) return bad(key, *v);
        out = static_cast<u32>(x);
        return true;
    }
    bool i(std::string_view key, i32& out) {
        const std::string* v = n.attr(key);
        if (!v) return true;
        i64 x = 0;
        if (!to_i64(*v, x) || x < INT32_MIN || x > INT32_MAX) return bad(key, *v);
        out = static_cast<i32>(x);
        return true;
    }
    bool f(std::string_view key, f64& out) {
        const std::string* v = n.attr(key);
        if (!v) return true;
        if (!to_f64(*v, out)) return bad(key, *v);
        return true;
    }
    bool b(std::string_view key, bool& out) {
        const std::string* v = n.attr(key);
        if (!v) return true;
        if (*v == "1" || *v == "true") out = true;
        else if (*v == "0" || *v == "false") out = false;
        else return bad(key, *v);
        return true;
    }
    bool bad(std::string_view key, const std::string& v) {
        error = where + ": " + std::string(key) + "=\"" + v + "\" — не число (строка " + std::to_string(n.line) + ")";
        return false;
    }
};

// A path as Tiled writes it, relative to the file it is in.
bool resolve(const fs::path& base_file, const std::string& source, fs::path& out) {
    if (source.empty() || source.find("://") != std::string::npos || source.rfind("qrc:", 0) == 0 || source.rfind("ext:", 0) == 0)
        return false;
    const fs::path p = utf8_path(source);
    out = (p.is_absolute() ? p : base_file.parent_path() / p).lexically_normal();
    return true;
}

void read_properties(const Node& n, std::vector<Property>& out) {
    for (const Node& c : n.children) {
        if (c.name != "properties") continue;
        for (const Node& p : c.children) {
            if (p.name != "property") continue;
            Property prop;
            prop.name = p.str("name");
            prop.type = p.str("type", "string");
            // A string of several lines is the element's text.
            prop.value = p.attr("value") ? p.str("value") : p.text;
            // A later one of the same name (from an object over its template) wins.
            auto same = std::find_if(out.begin(), out.end(), [&](const Property& q) { return q.name == prop.name; });
            if (same != out.end()) *same = std::move(prop);
            else out.push_back(std::move(prop));
        }
    }
}

const Node* child(const Node& n, std::string_view name) {
    for (const Node& c : n.children)
        if (c.name == name) return &c;
    return nullptr;
}

// --- tile layer data ---

bool base64(std::string_view text, std::vector<u8>& out) {
    out.clear();
    u32 acc = 0;
    int bits = 0, pad = 0;
    for (const char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') {
            ++pad;
            continue;
        } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else return false;
        if (pad) return false; // data after the padding
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>(acc >> bits));
        }
    }
    return pad <= 2;
}

// gzip: a header, then plain deflate (RFC 1952).
bool gunzip(const std::vector<u8>& in, std::vector<u8>& out) {
    if (in.size() < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return false;
    const u8 flags = in[3];
    usize at = 10;
    if (flags & 4) { // FEXTRA
        if (at + 2 > in.size()) return false;
        at += 2 + (in[at] | (in[at + 1] << 8));
    }
    if (flags & 8) // FNAME
        while (at < in.size() && in[at++] != 0) {}
    if (flags & 16) // FCOMMENT
        while (at < in.size() && in[at++] != 0) {}
    if (flags & 2) at += 2; // FHCRC
    if (at + 8 > in.size()) return false;
    return assets::inflate({in.data() + at, in.size() - at - 8}, false, out);
}

bool read_gids(const std::string& encoding, const std::string& compression, const Node& holder, usize count,
               std::vector<u32>& gids, std::string& error, const std::string& where) {
    gids.clear();
    if (encoding.empty()) {
        // The old way: a <tile gid> per cell.
        for (const Node& t : holder.children) {
            if (t.name != "tile") continue;
            i64 g = 0;
            if (t.attr("gid") && (!to_i64(t.str("gid"), g) || g < 0 || g > 0xFFFFFFFFll)) {
                error = where + ": gid=\"" + t.str("gid") + "\" — не число";
                return false;
            }
            gids.push_back(static_cast<u32>(g));
        }
    } else if (encoding == "csv") {
        std::string_view s = holder.text;
        usize at = 0;
        while (at < s.size()) {
            usize comma = s.find(',', at);
            if (comma == std::string_view::npos) comma = s.size();
            const std::string_view item = s.substr(at, comma - at);
            if (item.find_first_not_of(" \t\r\n") != std::string_view::npos) {
                i64 g = 0;
                if (!to_i64(item, g) || g < 0 || g > 0xFFFFFFFFll) {
                    error = where + ": в CSV не число «" + std::string(item.substr(0, 20)) + "»";
                    return false;
                }
                gids.push_back(static_cast<u32>(g));
            }
            at = comma + 1;
        }
    } else if (encoding == "base64") {
        std::vector<u8> bytes, raw;
        if (!base64(holder.text, bytes)) {
            error = where + ": испорчен base64";
            return false;
        }
        if (compression.empty()) raw = std::move(bytes);
        else if (compression == "zlib") {
            if (!assets::inflate(bytes, true, raw)) {
                error = where + ": испорчены данные zlib";
                return false;
            }
        } else if (compression == "gzip") {
            if (!gunzip(bytes, raw)) {
                error = where + ": испорчены данные gzip";
                return false;
            }
        } else {
            error = where + ": сжатие «" + compression + "» не поддерживается (только без сжатия, zlib и gzip; в Tiled: Свойства карты → Формат слоя тайлов)";
            return false;
        }
        if (raw.size() % 4 != 0) {
            error = where + ": длина данных не кратна 4 байтам";
            return false;
        }
        gids.resize(raw.size() / 4);
        for (usize k = 0; k < gids.size(); ++k)
            gids[k] = static_cast<u32>(raw[k * 4]) | static_cast<u32>(raw[k * 4 + 1]) << 8 | static_cast<u32>(raw[k * 4 + 2]) << 16 |
                      static_cast<u32>(raw[k * 4 + 3]) << 24;
    } else {
        error = where + ": кодировка «" + encoding + "» не поддерживается";
        return false;
    }
    if (gids.size() != count) {
        error = where + ": клеток " + std::to_string(gids.size()) + ", а должно быть " + std::to_string(count);
        return false;
    }
    return true;
}

// --- tilesets ---

bool read_image(const Node& img, const fs::path& base, std::string& source, fs::path& file, u32& w, u32& h, bool* trans, u8* rgb,
                std::string& error, const std::string& where) {
    if (child(img, "data")) {
        error = where + ": картинка внутри файла не поддерживается — сохраните её отдельным файлом";
        return false;
    }
    source = img.str("source");
    if (!resolve(base, source, file)) {
        error = where + ": путь к картинке «" + source + "» не поддерживается";
        return false;
    }
    Attrs a{img, error, where};
    if (!a.u("width", w) || !a.u("height", h)) return false;
    if (trans) {
        const std::string t = img.str("trans");
        std::string hex = t.size() == 7 && t[0] == '#' ? t.substr(1) : t;
        if (!t.empty()) {
            u32 v = 0;
            const auto r = std::from_chars(hex.data(), hex.data() + hex.size(), v, 16);
            if (hex.size() != 6 || r.ec != std::errc() || r.ptr != hex.data() + hex.size()) {
                error = where + ": trans=\"" + t + "\" — не цвет";
                return false;
            }
            *trans = true;
            rgb[0] = static_cast<u8>(v >> 16);
            rgb[1] = static_cast<u8>(v >> 8);
            rgb[2] = static_cast<u8>(v);
        }
    }
    return true;
}

bool read_tileset_body(const Node& n, const fs::path& base, Tileset& ts, std::string& error) {
    const std::string where = "набор тайлов «" + n.str("name") + "»";
    Attrs a{n, error, where};
    ts.name = n.str("name");
    if (!a.u("tilewidth", ts.tile_w) || !a.u("tileheight", ts.tile_h) || !a.u("spacing", ts.spacing) || !a.u("margin", ts.margin) ||
        !a.u("tilecount", ts.count) || !a.u("columns", ts.columns))
        return false;
    ts.render_size = n.str("tilerendersize", "tile");
    ts.fill_mode = n.str("fillmode", "stretch");
    ts.alignment = n.str("objectalignment", "unspecified");
    read_properties(n, ts.props);
    if (const Node* off = child(n, "tileoffset")) {
        Attrs o{*off, error, where};
        if (!o.i("x", ts.offset_x) || !o.i("y", ts.offset_y)) return false;
    }
    if (const Node* img = child(n, "image")) {
        if (!read_image(*img, base, ts.image_source, ts.image, ts.image_w, ts.image_h, &ts.trans, ts.trans_rgb, error, where)) return false;
        // Older files say nothing of columns or count: from the picture, as Tiled does.
        const u32 step_w = ts.tile_w + ts.spacing, step_h = ts.tile_h + ts.spacing;
        if (ts.columns == 0 && step_w > 0 && ts.image_w + ts.spacing >= 2 * ts.margin)
            ts.columns = (ts.image_w - 2 * ts.margin + ts.spacing) / step_w;
        if (ts.count == 0 && step_h > 0 && ts.image_h + ts.spacing >= 2 * ts.margin)
            ts.count = ts.columns * ((ts.image_h - 2 * ts.margin + ts.spacing) / step_h);
    }
    for (const Node& t : n.children) {
        if (t.name != "tile") continue;
        Tile tile;
        Attrs ta{t, error, where};
        if (!ta.u("id", tile.id) || !ta.i("x", tile.x) || !ta.i("y", tile.y) || !ta.u("width", tile.w) || !ta.u("height", tile.h))
            return false;
        tile.type = t.attr("class") ? t.str("class") : t.str("type");
        read_properties(t, tile.props);
        if (const Node* img = child(t, "image"))
            if (!read_image(*img, base, tile.image_source, tile.image, tile.image_w, tile.image_h, nullptr, nullptr, error,
                            where + ", тайл " + std::to_string(tile.id)))
                return false;
        if (const Node* og = child(t, "objectgroup"))
            tile.collision = std::any_of(og->children.begin(), og->children.end(), [](const Node& o) { return o.name == "object"; });
        if (const Node* an = child(t, "animation"))
            for (const Node& f : an->children)
                if (f.name == "frame" && !tile.animated) {
                    Attrs fa{f, error, where + ", тайл " + std::to_string(tile.id)};
                    if (!fa.u("tileid", tile.first_frame)) return false;
                    tile.animated = true;
                }
        ts.tiles.push_back(std::move(tile));
    }
    std::stable_sort(ts.tiles.begin(), ts.tiles.end(), [](const Tile& x, const Tile& y) { return x.id < y.id; });
    return true;
}

// The map's or a template's <tileset>: in the file, or a .tsx (read once per file).
bool read_tileset(const Node& n, const fs::path& base, Tileset& ts, std::string& error,
                  std::vector<std::pair<fs::path, std::shared_ptr<Tileset>>>& tsx_cache) {
    Attrs a{n, error, "набор тайлов"};
    if (!a.u("firstgid", ts.firstgid)) return false;
    if (ts.firstgid == 0) {
        error = "у набора тайлов «" + n.str("name", n.str("source")) + "» нет firstgid";
        return false;
    }
    ts.source = n.str("source");
    if (ts.source.empty()) return read_tileset_body(n, base, ts, error);
    // An external tileset: a missing or broken one is told, not fatal.
    const u32 firstgid = ts.firstgid;
    const std::string source = ts.source;
    if (source.ends_with(".tsj") || source.ends_with(".json")) {
        error = "набор тайлов «" + source + "» в формате JSON: Forge читает .tsx — сохраните набор в Tiled как .tsx";
        return false;
    }
    if (!resolve(base, source, ts.file)) {
        ts.missing = "путь «" + source + "» не поддерживается";
        return true;
    }
    for (const auto& [file, cached] : tsx_cache)
        if (file == ts.file) {
            ts = *cached;
            ts.firstgid = firstgid;
            ts.source = source;
            return true;
        }
    std::error_code ec;
    if (!fs::is_regular_file(ts.file, ec)) {
        ts.missing = "нет файла " + source;
    } else {
        Node root;
        std::string why;
        if (!read_xml(ts.file, root, why)) ts.missing = why;
        else if (root.name != "tileset") ts.missing = source + ": это не набор тайлов Tiled (<" + root.name + ">)";
        else if (!read_tileset_body(root, ts.file, ts, why)) ts.missing = source + ": " + why;
    }
    if (!ts.missing.empty()) {
        // Keep only what the map said.
        Tileset bare;
        bare.firstgid = firstgid;
        bare.source = source;
        bare.file = ts.file;
        bare.missing = ts.missing;
        const std::string stem = path_to_utf8(utf8_path(source).stem());
        bare.name = stem.empty() ? source : stem;
        ts = std::move(bare);
    }
    tsx_cache.emplace_back(ts.file, std::make_shared<Tileset>(ts));
    return true;
}

// --- objects ---

struct Reader {
    Map& map;
    std::string& error;
    std::vector<std::pair<fs::path, std::shared_ptr<Tileset>>> tsx;
    // Templates: the file, its object, and where its tileset is in the map.
    struct Template {
        fs::path file;
        std::string missing;
        Node object;
        Tileset tileset; // firstgid as in the .tx
        bool has_tileset = false;
    };
    std::vector<Template> templates;

    const Template& load_template(const fs::path& file, const std::string& source) {
        for (const Template& t : templates)
            if (t.file == file) return t;
        Template t;
        t.file = file;
        Node root;
        std::string why;
        std::error_code ec;
        if (source.ends_with(".tj") || source.ends_with(".json")) t.missing = "шаблон в формате JSON не поддерживается: " + source;
        else if (!fs::is_regular_file(file, ec)) t.missing = "нет файла " + source;
        else if (!read_xml(file, root, why)) t.missing = why;
        else if (root.name != "template" || !child(root, "object")) t.missing = source + ": это не шаблон объекта Tiled";
        else {
            t.object = *child(root, "object");
            if (const Node* tsn = child(root, "tileset")) {
                if (!read_tileset(*tsn, file, t.tileset, why, tsx)) t.missing = source + ": " + why;
                else t.has_tileset = true;
            }
        }
        templates.push_back(std::move(t));
        return templates.back();
    }

    // A template's gid as a gid of the map: its tileset found in the map by file (or added after the others).
    u32 map_gid(const Template& t, u32 gid) {
        if (!t.has_tileset || gid == 0) return gid;
        const u32 flags = gid & kGidFlags, local = (gid & ~kGidFlags) - t.tileset.firstgid;
        for (const Tileset& ts : map.tilesets)
            if (!ts.source.empty() && ts.file == t.tileset.file) return (ts.firstgid + local) | flags;
        Tileset added = t.tileset;
        u32 next = 1;
        for (const Tileset& ts : map.tilesets)
            next = std::max(next, ts.firstgid + std::max<u32>(ts.count, ts.tiles.empty() ? 1 : ts.tiles.back().id + 1));
        added.firstgid = next;
        map.tilesets.push_back(added);
        return (next + local) | flags;
    }

    bool object_fields(const Node& o, Object& obj, const std::string& where) {
        Attrs a{o, error, where};
        if (!a.u("id", obj.id) || !a.f("x", obj.x) || !a.f("y", obj.y) || !a.f("width", obj.w) || !a.f("height", obj.h) ||
            !a.f("rotation", obj.rotation) || !a.u("gid", obj.gid) || !a.b("visible", obj.visible))
            return false;
        if (o.attr("name")) obj.name = o.str("name");
        if (o.attr("class")) obj.type = o.str("class");
        else if (o.attr("type")) obj.type = o.str("type");
        if (child(o, "ellipse")) obj.shape = Object::Shape::Ellipse;
        else if (child(o, "point")) obj.shape = Object::Shape::Point;
        else if (child(o, "polygon")) obj.shape = Object::Shape::Polygon;
        else if (child(o, "polyline")) obj.shape = Object::Shape::Polyline;
        else if (child(o, "text")) obj.shape = Object::Shape::Text;
        else if (child(o, "capsule")) obj.shape = Object::Shape::Capsule;
        read_properties(o, obj.props);
        return true;
    }

    bool object(const Node& o, const fs::path& base, Object& obj) {
        const std::string where = "объект " + o.str("id") + (o.attr("name") ? " «" + o.str("name") + "»" : "");
        obj.template_source = o.str("template");
        if (!obj.template_source.empty()) {
            fs::path file;
            if (!resolve(base, obj.template_source, file)) obj.template_missing = "путь «" + obj.template_source + "» не поддерживается";
            else {
                const Template& t = load_template(file, obj.template_source);
                obj.template_missing = t.missing;
                if (t.missing.empty()) {
                    // The template first, then what the object has of its own.
                    if (!object_fields(t.object, obj, where)) return false;
                    obj.gid = map_gid(t, obj.gid);
                }
            }
        }
        const Object::Shape template_shape = obj.shape;
        obj.shape = Object::Shape::Rect;
        if (!object_fields(o, obj, where)) return false;
        const bool own_shape = child(o, "ellipse") || child(o, "point") || child(o, "polygon") || child(o, "polyline") ||
                               child(o, "text") || child(o, "capsule");
        if (!own_shape) obj.shape = template_shape;
        return true;
    }

    // --- layers ---

    struct Inherited {
        std::string path;
        bool visible = true;
        f64 opacity = 1, offset_x = 0, offset_y = 0, parallax_x = 1, parallax_y = 1;
        std::string tint;
    };

    bool layer_common(const Node& n, const Inherited& up, Layer& l, const std::string& where) {
        Attrs a{n, error, where};
        bool visible = true;
        f64 opacity = 1, ox = 0, oy = 0, px = 1, py = 1;
        if (!a.u("id", l.id) || !a.b("visible", visible) || !a.f("opacity", opacity) || !a.f("offsetx", ox) || !a.f("offsety", oy) ||
            !a.f("parallaxx", px) || !a.f("parallaxy", py))
            return false;
        l.name = up.path + n.str("name");
        l.type = n.str("class");
        l.visible = up.visible && visible;
        l.opacity = up.opacity * opacity;
        l.offset_x = up.offset_x + ox;
        l.offset_y = up.offset_y + oy;
        l.parallax_x = up.parallax_x * px;
        l.parallax_y = up.parallax_y * py;
        l.tint = n.attr("tintcolor") ? n.str("tintcolor") : up.tint;
        l.mode = n.str("mode");
        if (l.mode == "normal") l.mode.clear();
        read_properties(n, l.props);
        return true;
    }

    bool tile_layer(const Node& n, const Inherited& up) {
        Layer l;
        l.kind = Layer::Kind::Tiles;
        const std::string where = "слой «" + up.path + n.str("name") + "»";
        if (!layer_common(n, up, l, where)) return false;
        Attrs a{n, error, where};
        i32 x = 0, y = 0;
        u32 w = 0, h = 0;
        if (!a.i("x", x) || !a.i("y", y) || !a.u("width", w) || !a.u("height", h)) return false;
        const Node* data = child(n, "data");
        if (data) {
            const std::string encoding = data->str("encoding"), compression = data->str("compression");
            bool chunks = false;
            for (const Node& c : data->children) {
                if (c.name != "chunk") continue;
                chunks = true;
                Chunk ch;
                Attrs ca{c, error, where};
                if (!ca.i("x", ch.x) || !ca.i("y", ch.y) || !ca.u("width", ch.w) || !ca.u("height", ch.h)) return false;
                if (static_cast<u64>(ch.w) * ch.h > (1u << 24)) {
                    error = where + ": слишком большой кусок карты";
                    return false;
                }
                if (!read_gids(encoding, compression, c, static_cast<usize>(ch.w) * ch.h, ch.gids, error,
                               where + ", кусок " + std::to_string(ch.x) + "," + std::to_string(ch.y)))
                    return false;
                l.chunks.push_back(std::move(ch));
            }
            if (!chunks) {
                if (static_cast<u64>(w) * h > (1u << 26)) {
                    error = where + ": слишком большой слой";
                    return false;
                }
                Chunk ch;
                ch.x = x;
                ch.y = y;
                ch.w = w;
                ch.h = h;
                if (!read_gids(encoding, compression, *data, static_cast<usize>(w) * h, ch.gids, error, where)) return false;
                l.chunks.push_back(std::move(ch));
            }
        }
        map.layers.push_back(std::move(l));
        return true;
    }

    bool object_layer(const Node& n, const Inherited& up) {
        Layer l;
        l.kind = Layer::Kind::Objects;
        const std::string where = "слой «" + up.path + n.str("name") + "»";
        if (!layer_common(n, up, l, where)) return false;
        for (const Node& o : n.children) {
            if (o.name != "object") continue;
            Object obj;
            if (!object(o, map.file, obj)) return false;
            l.objects.push_back(std::move(obj));
        }
        map.layers.push_back(std::move(l));
        return true;
    }

    bool image_layer(const Node& n, const Inherited& up) {
        Layer l;
        l.kind = Layer::Kind::Image;
        if (!layer_common(n, up, l, "слой «" + up.path + n.str("name") + "»")) return false;
        if (const Node* img = child(n, "image")) l.image_source = img->str("source");
        map.layers.push_back(std::move(l));
        return true;
    }

    bool layers(const Node& parent, const Inherited& up, u32 depth) {
        for (const Node& c : parent.children) {
            if (c.name == "layer") {
                if (!tile_layer(c, up)) return false;
            } else if (c.name == "objectgroup") {
                if (!object_layer(c, up)) return false;
            } else if (c.name == "imagelayer") {
                if (!image_layer(c, up)) return false;
            } else if (c.name == "group") {
                if (depth > 32) {
                    error = "слишком глубоко вложенные группы слоёв";
                    return false;
                }
                Layer g;
                if (!layer_common(c, up, g, "группа «" + up.path + c.str("name") + "»")) return false;
                Inherited down;
                down.path = g.name + "/";
                down.visible = g.visible;
                down.opacity = g.opacity;
                down.offset_x = g.offset_x;
                down.offset_y = g.offset_y;
                down.parallax_x = g.parallax_x;
                down.parallax_y = g.parallax_y;
                down.tint = g.tint;
                if (!layers(c, down, depth + 1)) return false;
            }
        }
        return true;
    }
};

} // namespace

const Property* find_property(const std::vector<Property>& props, std::string_view name) {
    for (const Property& p : props)
        if (p.name == name) return &p;
    return nullptr;
}

bool is_true(const std::vector<Property>& props, std::string_view name) {
    const Property* p = find_property(props, name);
    return p && p->type == "bool" && p->value == "true";
}

const Tile* Tileset::tile(u32 id) const {
    auto it = std::lower_bound(tiles.begin(), tiles.end(), id, [](const Tile& t, u32 v) { return t.id < v; });
    return it != tiles.end() && it->id == id ? &*it : nullptr;
}

bool Tileset::has(u32 id) const {
    if (!missing.empty()) return false;
    if (!collection()) return id < count;
    const Tile* t = tile(id);
    return t && !t->image_source.empty();
}

const Tileset* tileset_of(const Map& map, u32 gid, u32& local) {
    gid &= ~kGidFlags;
    if (gid == 0) return nullptr;
    const Tileset* best = nullptr;
    for (const Tileset& ts : map.tilesets)
        if (ts.firstgid <= gid && (!best || ts.firstgid > best->firstgid)) best = &ts;
    if (best) local = gid - best->firstgid;
    return best;
}

bool read_map(const fs::path& tmx, Map& out, std::string* error) {
    out = {};
    out.file = tmx;
    std::string why;
    auto fail = [&](const std::string& w) {
        if (error) *error = w;
        return false;
    };
    const std::string ext = path_to_utf8(tmx.extension());
    if (ext == ".tmj" || ext == ".json") return fail("карта в формате JSON не поддерживается — сохраните её в Tiled как .tmx");
    Node root;
    if (!read_xml(tmx, root, why)) return fail(why);
    if (root.name != "map") return fail(path_to_utf8(tmx.filename()) + ": это не карта Tiled (<" + root.name + ">, а нужен <map>)");
    Attrs a{root, why, "карта"};
    if (!a.u("width", out.w) || !a.u("height", out.h) || !a.u("tilewidth", out.tile_w) || !a.u("tileheight", out.tile_h) ||
        !a.b("infinite", out.infinite))
        return fail(why);
    out.version = root.str("version");
    out.tiled_version = root.str("tiledversion");
    out.orientation = root.str("orientation", "orthogonal");
    out.render_order = root.str("renderorder", "right-down");
    out.background = root.str("backgroundcolor");
    read_properties(root, out.props);
    Reader r{out, why, {}, {}};
    for (const Node& c : root.children) {
        if (c.name != "tileset") continue;
        Tileset ts;
        if (!read_tileset(c, tmx, ts, why, r.tsx)) return fail(why);
        out.tilesets.push_back(std::move(ts));
    }
    std::stable_sort(out.tilesets.begin(), out.tilesets.end(), [](const Tileset& x, const Tileset& y) { return x.firstgid < y.firstgid; });
    if (!r.layers(root, {}, 0)) return fail(why);
    return true;
}

} // namespace forge::level::tiled
