#include "forge/pixel/svg.h"

#include <algorithm>

namespace forge::pixel {

namespace {

bool lower_eq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        if (x != b[i]) return false;
    }
    return true;
}

bool hex_digits(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}

} // namespace

std::vector<std::string> unsupported(const SvgOutline& svg) {
    std::vector<std::string> out;
    for (usize k = 0; k < svg.elements.size(); ++k) {
        const i32 i = static_cast<i32>(k);
        const SvgElement& e = svg.elements[k];
        const std::string who = svg.name_of(i);
        const std::string& t = e.tag;
        if (t == "marker") out.push_back(who + ": маркеры <marker> ThorVG не рисует");
        if (t == "svg" && e.parent >= 0) out.push_back(who + ": вложенный <svg> ThorVG рисует не так, как браузер");
        if (t == "text" || t == "tspan" || t == "textPath") out.push_back(who + ": текст <" + t + "> в этой версии не рисуется — переведите его в кривые");
        if (t == "image") out.push_back(who + ": вставленные картинки <image> в этой версии не рисуются");
        if (t == "foreignObject" || t == "script") out.push_back(who + ": <" + t + "> не рисуется");
        if (t == "animate" || t == "animateTransform" || t == "animateMotion" || t == "set")
            out.push_back(who + ": анимация <" + t + "> не играет — движение задаёт скрипт позы");
        if (e.parent >= 0 && svg.elements[static_cast<usize>(e.parent)].tag == "filter" && t != "feGaussianBlur")
            out.push_back(who + ": из фильтров ThorVG рисует только размытие, <" + t + "> не нарисуется");
        for (const char* m : {"marker-start", "marker-mid", "marker-end", "marker"})
            if (t != "marker" && !e.property(m).empty() && e.property(m) != "none") out.push_back(who + ": " + m + " — маркеры ThorVG не рисует");
        if (svg.inside(i, "clipPath") && lower_eq(e.property("clip-rule"), "evenodd"))
            out.push_back(who + ": clip-rule=\"evenodd\" в <clipPath> — ThorVG обрежет по правилу nonzero");
        for (const char* p : {"fill", "stroke", "stop-color", "color", "flood-color"}) {
            const std::string v = e.property(p);
            const bool short_alpha = v.size() == 5 && v[0] == '#' && hex_digits(std::string_view(v).substr(1));
            const bool long_alpha = v.size() == 9 && v[0] == '#' && hex_digits(std::string_view(v).substr(1));
            const bool hsla = v.size() > 5 && lower_eq(std::string_view(v).substr(0, 5), "hsla(");
            if (short_alpha || long_alpha || hsla) out.push_back(who + ": цвет " + p + "=\"" + v + "\" — такую запись ThorVG не читает");
        }
    }
    return out;
}

} // namespace forge::pixel
