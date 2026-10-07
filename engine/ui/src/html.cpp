#include "forge/ui/html.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace forge::ui {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool is_blank(std::string_view text) {
    for (char c : text)
        if (!is_space(c)) return false;
    return true;
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string lower_copy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower(c);
    return out;
}

bool starts_with_ci(std::string_view text, usize pos, std::string_view prefix) {
    if (pos + prefix.size() > text.size()) return false;
    for (usize i = 0; i < prefix.size(); ++i)
        if (lower(text[pos + i]) != prefix[i]) return false;
    return true;
}

bool is_void(std::string_view tag) {
    static constexpr std::array<std::string_view, 14> kVoid = {"area", "base", "br",    "col",  "embed", "hr",    "img",
                                                               "input", "link", "meta", "param", "source", "track", "wbr"};
    return std::find(kVoid.begin(), kVoid.end(), tag) != kVoid.end();
}

std::string escape_attr(std::string_view v) {
    std::string out;
    for (char c : v) {
        if (c == '"') out += "&quot;";
        else if (c == '<') out += "&lt;";
        else out += c;
    }
    return out;
}

struct Attr {
    std::string name;
    std::string value;
};

struct Tag {
    bool closing = false;
    bool self_closed = false;
    std::string name; // lower case
    std::vector<Attr> attrs;
};

// Reads a tag starting at text[pos] == '<'; pos ends after '>'.
bool read_tag(std::string_view text, usize& pos, Tag& tag) {
    usize p = pos + 1;
    if (p < text.size() && text[p] == '/') {
        tag.closing = true;
        ++p;
    }
    const usize name_start = p;
    while (p < text.size() && !is_space(text[p]) && text[p] != '>' && text[p] != '/') ++p;
    if (p == name_start) return false;
    tag.name = lower_copy(text.substr(name_start, p - name_start));
    while (p < text.size()) {
        while (p < text.size() && is_space(text[p])) ++p;
        if (p >= text.size()) return false;
        if (text[p] == '>') {
            pos = p + 1;
            return true;
        }
        if (text[p] == '/') {
            tag.self_closed = true;
            ++p;
            continue;
        }
        const usize an = p;
        while (p < text.size() && !is_space(text[p]) && text[p] != '=' && text[p] != '>' &&
               !(text[p] == '/' && p + 1 < text.size() && text[p + 1] == '>'))
            ++p;
        Attr a;
        a.name = lower_copy(text.substr(an, p - an));
        while (p < text.size() && is_space(text[p])) ++p;
        if (p < text.size() && text[p] == '=') {
            ++p;
            while (p < text.size() && is_space(text[p])) ++p;
            if (p < text.size() && (text[p] == '"' || text[p] == '\'')) {
                const char q = text[p++];
                const usize vs = p;
                while (p < text.size() && text[p] != q) ++p;
                a.value = std::string(text.substr(vs, p - vs));
                if (p < text.size()) ++p;
            } else {
                const usize vs = p;
                while (p < text.size() && !is_space(text[p]) && text[p] != '>') ++p;
                a.value = std::string(text.substr(vs, p - vs));
            }
        } else {
            a.value = a.name; // <input disabled> is disabled="disabled"
        }
        if (!a.name.empty()) tag.attrs.push_back(std::move(a));
    }
    return false;
}

std::string write_tag(const Tag& tag, bool self_close) {
    std::string out = tag.closing ? "</" : "<";
    out += tag.name;
    for (const Attr& a : tag.attrs) out += " " + a.name + "=\"" + escape_attr(a.value) + "\"";
    out += self_close ? "/>" : ">";
    return out;
}

const std::string* attr(const Tag& tag, std::string_view name) {
    for (const Attr& a : tag.attrs)
        if (a.name == name) return &a.value;
    return nullptr;
}

} // namespace

std::string html_to_rml(std::string_view html, std::string_view ua_sheet) {
    std::string body;
    body.reserve(html.size() + 256);
    bool has_head = false, has_body = false, has_root = false;
    // Outside <body> (before it or after it), when the page has one.
    bool in_root_only = lower_copy(html).find("<body") != std::string::npos;
    usize pos = 0;
    while (pos < html.size()) {
        const usize lt = html.find('<', pos);
        if (lt == std::string_view::npos) {
            body.append(html.substr(pos));
            break;
        }
        // Text outside <body> (the line breaks between <html>, <head> and <body>) is not part of the page.
        if (!(in_root_only && is_blank(html.substr(pos, lt - pos)))) body.append(html.substr(pos, lt - pos));
        pos = lt;
        if (starts_with_ci(html, pos, "<!--")) {
            const usize end = html.find("-->", pos + 4);
            const usize stop = end == std::string_view::npos ? html.size() : end + 3;
            body.append(html.substr(pos, stop - pos));
            pos = stop;
            continue;
        }
        if (starts_with_ci(html, pos, "<!") || starts_with_ci(html, pos, "<?")) { // doctype, xml declaration
            const usize end = html.find('>', pos);
            pos = end == std::string_view::npos ? html.size() : end + 1;
            continue;
        }
        Tag tag;
        usize after = pos;
        if (!read_tag(html, after, tag)) { // a lone '<' in text
            body += "&lt;";
            ++pos;
            continue;
        }
        pos = after;
        const std::string& n = tag.name;
        // Raw text elements: keep (style) or drop (script, title) up to their end tag.
        if (!tag.closing && (n == "style" || n == "script" || n == "title")) {
            const std::string end_tag = "</" + n;
            usize end = pos;
            while (end < html.size() && !starts_with_ci(html, end, end_tag)) ++end;
            const std::string_view content = html.substr(pos, end - pos);
            const usize close = html.find('>', end);
            pos = close == std::string_view::npos ? html.size() : close + 1;
            if (n == "style") {
                Tag open;
                open.name = "style";
                body += write_tag(open, false);
                body.append(content);
                body += "</style>";
            }
            continue;
        }
        if (n == "meta" || n == "base" || n == "script" || n == "title") continue;
        if (n == "html") {
            has_root = true;
            Tag t = tag;
            t.name = "rml";
            t.attrs.clear();
            body += write_tag(t, false);
            continue;
        }
        if (n == "head" && !tag.closing) has_head = true;
        if (n == "body") {
            has_body = has_body || !tag.closing;
            in_root_only = tag.closing;
        }
        if (n == "link" && !tag.closing) {
            const std::string* rel = attr(tag, "rel");
            const std::string* href = attr(tag, "href");
            if (rel && href && lower_copy(*rel) == "stylesheet") body += "<link type=\"text/rcss\" href=\"" + escape_attr(*href) + "\"/>";
            continue;
        }
        if (tag.closing && is_void(n)) continue; // </br> and the like
        body += write_tag(tag, tag.self_closed || is_void(n));
    }

    // Put the defaults first, and add what a fragment of a page leaves out.
    const std::string ua = ua_sheet.empty() ? std::string() : "<link type=\"text/rcss\" href=\"" + escape_attr(ua_sheet) + "\"/>";
    std::string out;
    if (has_head) {
        const usize head = body.find('>', body.find("<head")) + 1;
        out = body.substr(0, head) + ua + body.substr(head);
    } else if (has_body) {
        const usize b = body.find("<body");
        out = body.substr(0, b) + "<head>" + ua + "</head>" + body.substr(b);
    } else {
        out = "<head>" + ua + "</head><body>" + body + "</body>";
    }
    if (!has_root) out = "<rml>" + out + "</rml>";
    return out;
}

} // namespace forge::ui
