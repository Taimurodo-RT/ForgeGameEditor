#include "forge/assets/svg.h"

#include "forge/core/types.h"
#include "forge/vector/svg.h"

#include <lunasvg.h>

#include <algorithm>
#include <array>

namespace forge::assets {

namespace {

// The elements lunasvg 3.5 builds (lunasvg's svgelement.cpp, elementid); any other one it skips with all inside it.
constexpr std::array<std::string_view, 24> kDrawn = {"a",       "circle",         "clipPath", "defs",     "ellipse", "g",
                                                     "image",   "line",           "linearGradient", "marker", "mask",  "path",
                                                     "pattern", "polygon",        "polyline", "radialGradient", "rect", "stop",
                                                     "style",   "svg",            "symbol",   "text",     "tspan",   "use"};

bool drawn(std::string_view tag) { return std::find(kDrawn.begin(), kDrawn.end(), tag) != kDrawn.end(); }

// What a filter primitive does, in the author's words.
std::string primitive(std::string_view tag) {
    if (tag == "feGaussianBlur") return "размытие";
    if (tag == "feDropShadow") return "тень";
    if (tag == "feOffset") return "сдвиг";
    if (tag == "feColorMatrix") return "смена цветов";
    if (tag == "feFlood") return "заливка";
    if (tag == "feBlend" || tag == "feComposite" || tag == "feMerge") return "наложение";
    return "<" + std::string(tag) + ">";
}

struct Found {
    std::string key, what, tail;
    u32 line = 0, more = 0;
};

struct Gaps {
    std::vector<Found> found;
    bool filters = false; // blur or shadow left out: the line says what the interface has for them

    void add(std::string key, std::string what, u32 line, std::string tail) {
        for (Found& f : found)
            if (f.key == key) {
                ++f.more;
                return;
            }
        found.push_back({std::move(key), std::move(what), std::move(tail), line, 0});
    }
};

Gaps scan(std::string_view text) {
    Gaps gaps;
    if (!lunasvg::Document::loadFromData(text.data(), text.size())) {
        const vector::SvgOutline outline = vector::scan_svg(text);
        gaps.add("unread", "lunasvg не читает этот файл", 0, outline.error.empty() ? std::string() : outline.error);
        return gaps;
    }
    const vector::SvgOutline svg = vector::scan_svg(text);
    if (!svg.error.empty()) return gaps; // lunasvg reads more than the outline does; nothing to name then
    // What lunasvg skips with all inside it, so nothing inside is named again; editors' own elements and the
    // description are skipped quietly.
    std::vector<bool> skipped(svg.elements.size(), false);
    for (usize k = 0; k < svg.elements.size(); ++k) {
        const vector::SvgElement& e = svg.elements[k];
        if (e.parent >= 0 && skipped[static_cast<usize>(e.parent)]) {
            skipped[k] = true;
            continue;
        }
        const std::string& t = e.tag;
        const bool foreign = !e.prefix.empty() && e.prefix != "svg";
        if (foreign || t == "title" || t == "desc" || t == "metadata") {
            skipped[k] = true;
            continue;
        }
        const std::string where = e.prefix.empty() ? "<" + t + ">" : "<" + e.prefix + ":" + t + ">";
        if (!e.prefix.empty() || !drawn(t)) {
            skipped[k] = true;
            if (t == "filter" && e.prefix.empty()) {
                std::vector<std::string> words;
                for (usize c = k + 1; c < svg.elements.size(); ++c) {
                    if (svg.elements[c].parent != static_cast<i32>(k)) continue;
                    const std::string w = primitive(svg.elements[c].tag);
                    if (std::find(words.begin(), words.end(), w) == words.end()) words.push_back(w);
                }
                std::string what = e.id.empty() ? "фильтр" : "фильтр «" + e.id + "»";
                if (!words.empty()) {
                    what += " (";
                    for (usize w = 0; w < words.size(); ++w) what += (w ? ", " : "") + words[w];
                    what += ")";
                }
                gaps.add("filter#" + std::to_string(k), what, e.line, "lunasvg не рисует");
                gaps.filters = true;
            } else if (t == "animate" || t == "animateTransform" || t == "animateMotion" || t == "animateColor" || t == "set") {
                gaps.add("animation", "анимация " + where, e.line, "не играет");
            } else {
                const bool kids = std::any_of(svg.elements.begin() + static_cast<std::ptrdiff_t>(k) + 1, svg.elements.end(),
                                              [&](const vector::SvgElement& c) { return c.parent == static_cast<i32>(k); });
                gaps.add("tag:" + where, where, e.line, kids ? "lunasvg не рисует, и всё внутри тоже" : "lunasvg не рисует");
            }
            continue;
        }
        if (t == "text") gaps.add("text", "текст <text>", e.line, "lunasvg рисует шрифтом компьютера игрока — надёжнее перевести его в кривые");
        if (t == "image") {
            const std::string* href = e.attr("href");
            if (!href) href = e.attr("xlink:href");
            if (href && href->rfind("data:", 0) != 0)
                gaps.add("image:" + *href, "картинка <image> «" + *href + "»", e.line, "не загрузится: lunasvg берёт только встроенные (data:)");
        }
        const std::string filter = e.property("filter");
        if (!filter.empty() && filter != "none" && filter.rfind("url(", 0) != 0) {
            gaps.add("css-filter", "filter: " + filter, e.line, "lunasvg не рисует");
            gaps.filters = true;
        }
        const std::string blend = e.property("mix-blend-mode");
        if (!blend.empty() && blend != "normal") gaps.add("blend", "наложение mix-blend-mode: " + blend, e.line, "lunasvg не рисует");
    }
    return gaps;
}

std::vector<std::string> clauses(const Gaps& gaps) {
    std::vector<std::string> out;
    for (const Found& f : gaps.found) {
        std::string clause = f.what;
        if (f.line > 0) clause += " в строке " + std::to_string(f.line);
        if (f.more > 0) clause += " (и ещё " + std::to_string(f.more) + ")";
        if (!f.tail.empty()) clause += (f.key == "unread" ? ": " : " ") + f.tail;
        out.push_back(std::move(clause));
    }
    return out;
}

} // namespace

bool svg_readable(std::span<const u8> bytes) {
    return !bytes.empty() && lunasvg::Document::loadFromData(reinterpret_cast<const char*>(bytes.data()), bytes.size()) != nullptr;
}

std::vector<std::string> svg_gaps(std::string_view text) { return clauses(scan(text)); }

std::string svg_gaps_line(std::string_view text) {
    const Gaps gaps = scan(text);
    std::string line;
    for (const std::string& clause : clauses(gaps)) line += (line.empty() ? "" : "; ") + clause;
    if (gaps.filters) line += "; размытие и тень интерфейсу дают свойства filter и box-shadow в RCSS";
    return line;
}

} // namespace forge::assets
