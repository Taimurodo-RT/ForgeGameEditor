#include "forge/vector/svg.h"

#include <charconv>

namespace forge::vector {

namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

std::string_view local_name(std::string_view name) {
    const usize colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

void put_utf8(std::string& out, u32 c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// The five entities of XML and character references; anything else stays as written.
std::string decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const usize end = s.find(';', i);
        if (end == std::string_view::npos || end - i > 10) {
            out += s[i];
            continue;
        }
        const std::string_view name = s.substr(i + 1, end - i - 1);
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() > 1 && name[0] == '#') {
            u32 c = 0;
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string_view digits = name.substr(hex ? 2 : 1);
            const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), c, hex ? 16 : 10);
            if (ec != std::errc() || p != digits.data() + digits.size() || c > 0x10FFFF) {
                out += s.substr(i, end - i + 1);
            } else {
                put_utf8(out, c);
            }
        } else {
            out += s.substr(i, end - i + 1);
        }
        i = end;
    }
    return out;
}

struct Scanner {
    std::string_view s;
    usize i = 0;
    u32 line = 1;
    SvgOutline out;
    std::vector<i32> open; // elements not closed yet, innermost last

    bool at(std::string_view w) const { return s.substr(i, w.size()) == w; }
    void step(usize n) {
        for (usize k = 0; k < n && i < s.size(); ++k, ++i)
            if (s[i] == '\n') ++line;
    }
    void skip_space() {
        while (i < s.size() && space(s[i])) step(1);
    }
    bool fail(const std::string& why) {
        out.error = "строка " + std::to_string(line) + ": " + why;
        return false;
    }
    // Up to and past `end`; false when the file ends first.
    bool skip_past(std::string_view end, const char* what) {
        const usize at_end = s.find(end, i);
        if (at_end == std::string_view::npos) return fail(std::string(what) + " не закрыт");
        step(at_end + end.size() - i);
        return true;
    }
    std::string_view name() {
        const usize from = i;
        while (i < s.size() && !space(s[i]) && s[i] != '/' && s[i] != '>' && s[i] != '=') ++i;
        return s.substr(from, i - from);
    }

    bool doctype() {
        // <!DOCTYPE …> may hold an internal subset in brackets.
        int depth = 0;
        while (i < s.size()) {
            const char c = s[i];
            step(1);
            if (c == '[') ++depth;
            else if (c == ']') --depth;
            else if (c == '>' && depth <= 0) return true;
        }
        return fail("<!DOCTYPE> не закрыт");
    }

    bool start_tag() {
        step(1); // <
        const u32 at_line = line;
        const std::string_view tag = name();
        if (tag.empty()) return fail("после «<» нет имени элемента");
        SvgElement e;
        e.tag = std::string(local_name(tag));
        if (tag.size() > e.tag.size()) e.prefix = std::string(tag.substr(0, tag.size() - e.tag.size() - 1));
        e.line = at_line;
        e.parent = open.empty() ? -1 : open.back();
        if (open.empty() && !out.elements.empty()) return fail("второй корневой элемент <" + e.tag + ">");
        for (;;) {
            skip_space();
            if (i >= s.size()) return fail("элемент <" + e.tag + "> не закрыт");
            if (at("/>")) {
                step(2);
                out.elements.push_back(std::move(e));
                return true;
            }
            if (s[i] == '>') {
                step(1);
                open.push_back(static_cast<i32>(out.elements.size()));
                out.elements.push_back(std::move(e));
                return true;
            }
            const std::string_view attr = name();
            if (attr.empty()) return fail("в <" + e.tag + "> лишний знак «" + std::string(1, s[i]) + "»");
            skip_space();
            if (i >= s.size() || s[i] != '=') return fail("у атрибута «" + std::string(attr) + "» нет значения");
            step(1);
            skip_space();
            if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return fail("значение «" + std::string(attr) + "» не в кавычках");
            const char quote = s[i];
            step(1);
            const usize end = s.find(quote, i);
            if (end == std::string_view::npos) return fail("значение «" + std::string(attr) + "» не закрыто");
            std::string value = decode(s.substr(i, end - i));
            step(end + 1 - i);
            if (attr == "id") e.id = value;
            e.attrs.emplace_back(std::string(attr), std::move(value));
        }
    }

    bool end_tag() {
        step(2); // </
        const std::string_view tag = local_name(name());
        skip_space();
        if (i >= s.size() || s[i] != '>') return fail("</" + std::string(tag) + " не закрыт");
        step(1);
        if (open.empty() || out.elements[static_cast<usize>(open.back())].tag != tag)
            return fail("</" + std::string(tag) + "> закрывает не тот элемент" +
                        (open.empty() ? std::string() : " (открыт <" + out.elements[static_cast<usize>(open.back())].tag + ">)"));
        open.pop_back();
        return true;
    }

    bool run() {
        while (i < s.size()) {
            if (s[i] != '<') {
                step(1);
                continue;
            }
            bool ok;
            if (at("<!--")) ok = skip_past("-->", "комментарий");
            else if (at("<![CDATA[")) ok = skip_past("]]>", "CDATA");
            else if (at("<?")) ok = skip_past("?>", "<?…?>");
            else if (at("<!")) ok = doctype();
            else if (at("</")) ok = end_tag();
            else ok = start_tag();
            if (!ok) return false;
        }
        if (!open.empty()) return fail("элемент <" + out.elements[static_cast<usize>(open.back())].tag + "> не закрыт");
        if (out.elements.empty()) return fail("в файле нет ни одного элемента");
        if (out.elements.front().tag != "svg") return fail("это не SVG: корень — <" + out.elements.front().tag + ">");
        return true;
    }
};

} // namespace

const std::string* SvgElement::attr(std::string_view name) const {
    for (const auto& [k, v] : attrs)
        if (k == name) return &v;
    return nullptr;
}

std::string SvgElement::property(std::string_view name) const {
    if (const std::string* style = attr("style")) {
        std::string_view rest = *style;
        while (!rest.empty()) {
            const usize semi = rest.find(';');
            const std::string_view decl = rest.substr(0, semi);
            rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
            const usize colon = decl.find(':');
            if (colon != std::string_view::npos && trim(decl.substr(0, colon)) == name) return std::string(trim(decl.substr(colon + 1)));
        }
    }
    const std::string* v = attr(name);
    return v ? std::string(trim(*v)) : std::string();
}

i32 SvgOutline::find(std::string_view id) const {
    if (id.empty()) return -1;
    for (usize i = 0; i < elements.size(); ++i)
        if (elements[i].id == id) return static_cast<i32>(i);
    return -1;
}

std::string SvgOutline::name_of(i32 index) const {
    const SvgElement& e = elements[static_cast<usize>(index)];
    if (!e.id.empty()) return "«" + e.id + "»";
    return "<" + e.tag + "> в строке " + std::to_string(e.line);
}

SvgOutline scan_svg(std::string_view text) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3); // UTF-8 BOM
    Scanner sc;
    sc.s = text;
    if (!sc.run()) sc.out.elements.clear();
    return std::move(sc.out);
}

bool SvgOutline::inside(i32 index, std::string_view tag) const {
    for (i32 p = elements[static_cast<usize>(index)].parent; p >= 0; p = elements[static_cast<usize>(p)].parent)
        if (elements[static_cast<usize>(p)].tag == tag) return true;
    return false;
}

} // namespace forge::vector
