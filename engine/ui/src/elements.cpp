#include "elements.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/ui/drawing.h"
#include "forge/ui/virtual_list.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace forge::ui {

namespace {

std::unordered_map<std::string, u32> g_icons;
std::unordered_map<std::string, ListSource*> g_sources;
std::unordered_set<ElementVirtualList*> g_lists;
std::unordered_map<std::string, LineSource*> g_line_sources;

std::string utf8(u32 cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// Text goes into RML, so markup characters are escaped.
std::string escape_rml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

bool starts_with(const std::string& s, std::string_view prefix) { return s.rfind(prefix, 0) == 0; }

} // namespace

// ---------------------------------------------------------------------------
// Icons

bool load_icon_codepoints(const std::filesystem::path& file) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return false;
    g_icons.clear();
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    usize pos = 0;
    while (pos < text.size()) {
        usize end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        const usize space = line.find(' ');
        if (space != std::string_view::npos) {
            const std::string hex(line.substr(space + 1));
            g_icons[std::string(line.substr(0, space))] = static_cast<u32>(std::strtoul(hex.c_str(), nullptr, 16));
        }
        pos = end + 1;
    }
    return !g_icons.empty();
}

u32 icon_codepoint(std::string_view name) {
    auto it = g_icons.find(std::string(name));
    return it != g_icons.end() ? it->second : 0;
}

Rml::UnorderedMap<Rml::String, Rml::Character> icon_word_glyphs() {
    Rml::UnorderedMap<Rml::String, Rml::Character> words;
    words.reserve(g_icons.size());
    for (const auto& [name, codepoint] : g_icons) words[name] = static_cast<Rml::Character>(codepoint);
    return words;
}

ElementIcon::ElementIcon(const Rml::String& tag_name) : Rml::Element(tag_name) {}

void ElementIcon::OnAttributeChange(const Rml::ElementAttributes& changed) {
    Rml::Element::OnAttributeChange(changed);
    auto it = changed.find("name");
    if (it == changed.end()) return;
    const Rml::String name = it->second.Get<Rml::String>();
    const u32 cp = icon_codepoint(name);
    if (!cp && !name.empty()) FORGE_WARN("ui: unknown icon '%s'", name.c_str());
    SetInnerRML(cp ? utf8(cp) : Rml::String());
}

// ---------------------------------------------------------------------------
// Virtual list

void register_list_source(const std::string& name, ListSource* source) {
    if (source) g_sources[name] = source;
    else g_sources.erase(name);
}

ListSource* find_list_source(const std::string& name) {
    auto it = g_sources.find(name);
    return it != g_sources.end() ? it->second : nullptr;
}

void scroll_list_to(const std::string& source, u32 row) {
    for (ElementVirtualList* list : g_lists)
        if (list->source_name() == source) list->scroll_to_row(row);
}

ElementVirtualList::ElementVirtualList(const Rml::String& tag_name) : Rml::Element(tag_name) { g_lists.insert(this); }

ElementVirtualList::~ElementVirtualList() {
    g_lists.erase(this);
    if (built_) {
        RemoveEventListener("click", this);
        RemoveEventListener("dblclick", this);
        RemoveEventListener("mousedown", this);
    }
}

void ElementVirtualList::OnAttributeChange(const Rml::ElementAttributes& changed) {
    Rml::Element::OnAttributeChange(changed);
    if (auto it = changed.find("source"); it != changed.end()) {
        source_name_ = it->second.Get<Rml::String>();
        version_ = ~0ull;
    }
    if (auto it = changed.find("row-height"); it != changed.end())
        row_height_ = std::max(1.0f, it->second.Get<float>());
    if (auto it = changed.find("cell-width"); it != changed.end()) {
        const float w = std::max(0.0f, it->second.Get<float>());
        if (w != cell_width_) {
            cell_width_ = w;
            clear_rows(); // list rows and grid cells are placed differently
        }
    }
}

void ElementVirtualList::clear_rows() {
    if (content_)
        for (Row& row : rows_) content_->RemoveChild(row.element);
    rows_.clear();
    first_ = ~0u;
    count_ = ~0u;
    columns_ = 1;
}

void ElementVirtualList::scroll_to_row(u32 row) { pending_scroll_row_ = row; }

void ElementVirtualList::build() {
    built_ = true;
    template_rml_ = GetInnerRML();
    while (GetNumChildren() > 0) RemoveChild(GetChild(0));
    SetProperty("overflow-y", "auto");
    Rml::ElementPtr content = GetOwnerDocument()->CreateElement("div");
    content_ = AppendChild(std::move(content));
    content_->SetClass("vl-content", true);
    content_->SetProperty("display", "block");
    content_->SetProperty("position", "relative");
    // On the list itself: clicks outside the rows count too.
    AddEventListener("click", this);
    AddEventListener("dblclick", this);
    AddEventListener("mousedown", this);
}

void ElementVirtualList::bind_row(Row& row, u32 index, ListSource& source, bool force) {
    if (row.index == index && !force) return;
    row.index = index;
    for (Binding& b : row.bindings) {
        std::string value = source.field(index, b.field);
        if (b.set && value == b.value) continue;
        switch (b.kind) {
        case Binding::Text: b.element->SetInnerRML(escape_rml(value)); break;
        case Binding::Attribute: b.element->SetAttribute(b.target, value); break;
        case Binding::Class: b.element->SetClass(b.target, value == "1" || value == "true"); break;
        case Binding::Style:
            if (value.empty()) b.element->RemoveProperty(b.target);
            else b.element->SetProperty(b.target, value);
            break;
        }
        b.value = std::move(value);
        b.set = true;
    }
}

void ElementVirtualList::OnUpdate() {
    Rml::Element::OnUpdate();
    if (!built_) build();
    FORGE_ZONE_N("VirtualList");

    ListSource* source = find_list_source(source_name_);
    const u32 count = source ? source->count() : 0;
    const u64 version = source ? source->version() : 0;

    // Rows take the height the style sheet gives them.
    for (const Row& row : rows_) {
        if (row.index == ~0u) continue;
        const float h = row.element->GetOffsetHeight();
        if (h > 0.5f && std::fabs(h - row_height_) > 0.01f) {
            row_height_ = h;
            first_ = ~0u; // re-layout
            columns_ = 0;
            count_ = ~0u;
            for (Row& r : rows_) r.index = ~0u, r.element->SetProperty("display", "none");
        }
        break;
    }

    // A grid: as many columns as fit; cells share the width evenly.
    const float width = GetClientWidth();
    const u32 columns = cell_width_ > 0 ? std::max(1u, static_cast<u32>(width / cell_width_)) : 1u;
    const float cell_w = cell_width_ > 0 ? std::floor(width / static_cast<float>(columns)) : 0.0f;
    if (columns != columns_ || (cell_width_ > 0 && std::fabs(cell_w - cell_w_) > 0.5f)) {
        columns_ = columns;
        cell_w_ = cell_w;
        first_ = ~0u;
        count_ = ~0u;
        for (Row& r : rows_) r.index = ~0u, r.element->SetProperty("display", "none");
    }
    const float client = std::max(GetClientHeight(), row_height_);
    const u32 needed = (static_cast<u32>(std::ceil(client / row_height_)) + 1) * columns;
    while (rows_.size() < needed) {
        Row row;
        Rml::ElementPtr element = GetOwnerDocument()->CreateElement("div");
        row.element = content_->AppendChild(std::move(element));
        row.element->SetClass("vl-row", true);
        row.element->SetProperty("position", "absolute");
        row.element->SetProperty("left", "0px");
        if (cell_width_ <= 0) row.element->SetProperty("right", "0px");
        row.element->SetProperty("display", "none");
        row.element->SetInnerRML(template_rml_);
        // Collect the bindings of the template.
        std::vector<Rml::Element*> stack{row.element};
        while (!stack.empty()) {
            Rml::Element* e = stack.back();
            stack.pop_back();
            for (const auto& [name, value] : e->GetAttributes()) {
                if (!starts_with(name, "vl-")) continue;
                Binding b{e, Binding::Text, {}, value.Get<Rml::String>(), {}, false};
                if (name == "vl-text") b.kind = Binding::Text;
                else if (starts_with(name, "vl-attr-")) b.kind = Binding::Attribute, b.target = name.substr(8);
                else if (starts_with(name, "vl-class-")) b.kind = Binding::Class, b.target = name.substr(9);
                else if (starts_with(name, "vl-style-")) b.kind = Binding::Style, b.target = name.substr(9);
                else continue;
                row.bindings.push_back(std::move(b));
            }
            for (int i = 0; i < e->GetNumChildren(); ++i) stack.push_back(e->GetChild(i));
        }
        rows_.push_back(std::move(row));
        first_ = ~0u;
    }

    if (pending_scroll_row_ >= 0 && count > 0) {
        const float top = static_cast<float>(std::min<i64>(pending_scroll_row_, count - 1) / columns) * row_height_;
        if (top < GetScrollTop()) SetScrollTop(top);
        else if (top + row_height_ > GetScrollTop() + GetClientHeight())
            SetScrollTop(top + row_height_ - GetClientHeight());
        pending_scroll_row_ = -1;
    }

    u32 first = static_cast<u32>(std::max(0.0f, GetScrollTop()) / row_height_) * columns;
    if (count == 0) first = 0;
    else if (first >= count) first = (count - 1) / columns * columns;

    const bool layout_changed = first != first_ || count != count_;
    const bool data_changed = version != version_;
    if (!layout_changed && !data_changed) return;

    const u32 lines = (count + columns - 1) / columns;
    if (count != count_) content_->SetProperty("height", std::to_string(static_cast<double>(lines) * row_height_) + "px");

    // Rows are positioned absolutely at index * row height. A row whose
    // index is still on screen keeps it; only rows that scrolled out are
    // given the new indices, so scrolling by one row rebinds one row.
    const u32 window_end = std::min<u32>(count, first + static_cast<u32>(rows_.size()));
    std::vector<u8> taken(rows_.size(), 0);
    std::vector<Row*> free_rows;
    for (Row& row : rows_) {
        if (source && row.index != ~0u && row.index >= first && row.index < window_end) {
            taken[row.index - first] = 1;
            if (data_changed) bind_row(row, row.index, *source, true);
        } else {
            free_rows.push_back(&row);
        }
    }
    usize next_free = 0;
    for (u32 index = first; index < window_end; ++index) {
        if (taken[index - first]) continue;
        Row& row = *free_rows[next_free++];
        if (row.index == ~0u) row.element->RemoveProperty("display");
        row.element->SetProperty("top", std::to_string(static_cast<double>(index / columns) * row_height_) + "px");
        if (cell_width_ > 0) {
            row.element->SetProperty("left", std::to_string(static_cast<double>(index % columns) * cell_w) + "px");
            row.element->SetProperty("width", std::to_string(static_cast<double>(cell_w)) + "px");
        }
        bind_row(row, index, *source, data_changed);
    }
    for (; next_free < free_rows.size(); ++next_free) {
        Row& row = *free_rows[next_free];
        if (row.index != ~0u) {
            row.element->SetProperty("display", "none");
            row.index = ~0u;
        }
    }
    first_ = first;
    count_ = count;
    version_ = version;
}

void ElementVirtualList::ProcessEvent(Rml::Event& event) {
    ListSource* source = find_list_source(source_name_);
    if (!source) return;
    Rml::Element* e = event.GetTargetElement();
    while (e && e != this && e->GetParentNode() != content_) e = e->GetParentNode();
    u32 index = ListSource::kNoRow;
    if (e && e != this)
        for (const Row& row : rows_)
            if (row.element == e) index = row.index;
    int modifiers = 0;
    if (event.GetParameter<int>("ctrl_key", 0)) modifiers |= Rml::Input::KM_CTRL;
    if (event.GetParameter<int>("shift_key", 0)) modifiers |= Rml::Input::KM_SHIFT;
    if (event.GetParameter<int>("alt_key", 0)) modifiers |= Rml::Input::KM_ALT;
    if (event.GetType() == "mousedown") {
        const Rml::Element* t = event.GetTargetElement();
        if (event.GetParameter<int>("button", 0) == 1 && (index != ListSource::kNoRow || t == this || t == content_))
            source->on_context(index, event.GetParameter<float>("mouse_x", 0), event.GetParameter<float>("mouse_y", 0), modifiers);
        return;
    }
    if (index != ListSource::kNoRow) source->on_row_event(index, event.GetType(), modifiers);
    else if (event.GetType() == "click" && (event.GetTargetElement() == this || event.GetTargetElement() == content_))
        source->on_empty_click(modifiers); // not the scroll bar
}

// --- <lines> and <shape> ---------------------------------------------------------

void register_line_source(const std::string& name, LineSource* source) {
    if (source) g_line_sources[name] = source;
    else g_line_sources.erase(name);
}

void wire_curve(f32 x0, f32 y0, f32 x1, f32 y1, std::vector<f32>& out) {
    // The handles stick out sideways; further when the wire goes back left.
    const f32 dx = x1 - x0;
    const f32 reach = std::clamp(std::fabs(dx) * 0.5f + (dx < 0 ? 60.0f : 0.0f), 30.0f, 260.0f);
    const f32 c0x = x0 + reach, c1x = x1 - reach;
    const f32 len = std::fabs(dx) + std::fabs(y1 - y0) + 2 * reach;
    const int steps = std::clamp(static_cast<int>(len / 12.0f), 8, 64);
    for (int i = 0; i <= steps; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(steps), u = 1 - t;
        const f32 a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        out.push_back(a * x0 + b * c0x + c * c1x + d * x1);
        out.push_back(a * y0 + b * y0 + c * y1 + d * y1);
    }
}

f32 distance_to(const std::vector<f32>& p, f32 x, f32 y) {
    f32 best = 1e30f;
    for (usize i = 0; i + 3 < p.size(); i += 2) {
        const f32 ax = p[i], ay = p[i + 1], bx = p[i + 2], by = p[i + 3];
        const f32 vx = bx - ax, vy = by - ay;
        const f32 l2 = vx * vx + vy * vy;
        const f32 t = l2 > 0 ? std::clamp(((x - ax) * vx + (y - ay) * vy) / l2, 0.0f, 1.0f) : 0.0f;
        best = std::min(best, std::hypot(ax + vx * t - x, ay + vy * t - y));
    }
    return best;
}

namespace {

Rml::ColourbPremultiplied premultiplied(u32 rgba, f32 opacity) {
    const f32 a = static_cast<f32>(rgba & 0xff) * opacity;
    const auto ch = [&](u32 shift) { return static_cast<Rml::byte>(static_cast<f32>((rgba >> shift) & 0xff) * a / 255.0f + 0.5f); };
    return Rml::ColourbPremultiplied(ch(24), ch(16), ch(8), static_cast<Rml::byte>(a + 0.5f));
}

// A thick line along points with a soft pixel at each edge.
void add_polyline(Rml::Mesh& mesh, const f32* p, usize count, Rml::ColourbPremultiplied color, f32 width, bool closed) {
    if (count < 2) return;
    // Thin lines fade instead of thinning below a pixel.
    if (width < 1) {
        color = Rml::ColourbPremultiplied(static_cast<Rml::byte>(color.red * width), static_cast<Rml::byte>(color.green * width),
                                          static_cast<Rml::byte>(color.blue * width), static_cast<Rml::byte>(color.alpha * width));
        width = 1;
    }
    const f32 half = width * 0.5f - 0.5f, soft = half + 1.0f;
    const Rml::ColourbPremultiplied clear(0, 0, 0, 0);
    const int base = static_cast<int>(mesh.vertices.size());
    const usize n = closed ? count + 1 : count;
    for (usize k = 0; k < n; ++k) {
        const usize i = k % count;
        const usize prev = closed ? (i + count - 1) % count : (i == 0 ? 0 : i - 1);
        const usize next = closed ? (i + 1) % count : std::min(i + 1, count - 1);
        f32 tx = p[next * 2] - p[prev * 2], ty = p[next * 2 + 1] - p[prev * 2 + 1];
        const f32 tl = std::hypot(tx, ty);
        if (tl < 1e-4f) tx = 1, ty = 0;
        else tx /= tl, ty /= tl;
        // The normal, longer at corners so the width stays (up to a limit).
        f32 nx = -ty, ny = tx, scale = 1;
        if (prev != i && next != i) {
            f32 ax = p[i * 2] - p[prev * 2], ay = p[i * 2 + 1] - p[prev * 2 + 1];
            const f32 al = std::hypot(ax, ay);
            if (al > 1e-4f) {
                ax /= al, ay /= al;
                const f32 dot = -ay * nx + ax * ny;
                scale = 1.0f / std::max(0.4f, dot);
            }
        }
        const f32 x = p[i * 2], y = p[i * 2 + 1];
        const f32 offs[4] = {soft, half, -half, -soft};
        for (int j = 0; j < 4; ++j) {
            Rml::Vertex v;
            v.position = {x + nx * offs[j] * scale, y + ny * offs[j] * scale};
            v.colour = (j == 0 || j == 3) ? clear : color;
            v.tex_coord = {0, 0};
            mesh.vertices.push_back(v);
        }
    }
    for (usize k = 0; k + 1 < n; ++k) {
        const int a = base + static_cast<int>(k) * 4, b = a + 4;
        for (int j = 0; j < 3; ++j) {
            mesh.indices.insert(mesh.indices.end(), {a + j, b + j, b + j + 1, a + j, b + j + 1, a + j + 1});
        }
    }
}

} // namespace

ElementLines::ElementLines(const Rml::String& tag_name) : Rml::Element(tag_name) {}

void ElementLines::OnAttributeChange(const Rml::ElementAttributes& changed) {
    Rml::Element::OnAttributeChange(changed);
    if (changed.count("source")) {
        source_name_ = GetAttribute<Rml::String>("source", "");
        version_ = ~0ull;
    }
}

void ElementLines::OnRender() {
    const auto it = g_line_sources.find(source_name_);
    if (it == g_line_sources.end()) return;
    const LineSource& src = *it->second;
    if (src.version() != version_ || !geometry_) {
        version_ = src.version();
        Rml::Mesh mesh;
        const f32 opacity = GetComputedValues().opacity();
        for (const Line& l : src.lines())
            add_polyline(mesh, l.points.data(), l.points.size() / 2, premultiplied(l.color, opacity), l.width, false);
        geometry_ = mesh ? GetRenderManager()->MakeGeometry(std::move(mesh)) : Rml::Geometry();
    }
    if (geometry_) geometry_.Render(GetAbsoluteOffset(Rml::BoxArea::Border));
}

ElementShape::ElementShape(const Rml::String& tag_name) : Rml::Element(tag_name) {}

void ElementShape::OnAttributeChange(const Rml::ElementAttributes& changed) {
    Rml::Element::OnAttributeChange(changed);
    if (changed.count("fill") || changed.count("kind")) {
        const Rml::String fill = GetAttribute<Rml::String>("fill", "");
        filled_ = fill == "true" || fill == "1";
        dirty_ = true;
    }
}

void ElementShape::OnResize() { dirty_ = true; }

void ElementShape::OnPropertyChange(const Rml::PropertyIdSet& changed) {
    Rml::Element::OnPropertyChange(changed);
    if (changed.Contains(Rml::PropertyId::Color) || changed.Contains(Rml::PropertyId::Opacity)) dirty_ = true;
}

void ElementShape::OnRender() {
    if (dirty_) {
        dirty_ = false;
        const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
        const Rml::Colourb c = GetComputedValues().color();
        const u32 rgba = (static_cast<u32>(c.red) << 24) | (static_cast<u32>(c.green) << 16) | (static_cast<u32>(c.blue) << 8) | c.alpha;
        const Rml::ColourbPremultiplied color = premultiplied(rgba, GetComputedValues().opacity());
        // An arrow head: a box with a point to the right (Unreal's flow pin).
        const f32 w = size.x, h = size.y, in = 1.0f;
        const f32 pts[10] = {in, in, w * 0.55f, in, w - in, h * 0.5f, w * 0.55f, h - in, in, h - in};
        Rml::Mesh mesh;
        if (filled_) {
            add_polyline(mesh, pts, 5, color, 1.0f, true);
            const int base = static_cast<int>(mesh.vertices.size());
            for (int i = 0; i < 5; ++i) {
                Rml::Vertex v;
                v.position = {pts[i * 2], pts[i * 2 + 1]};
                v.colour = color;
                v.tex_coord = {0, 0};
                mesh.vertices.push_back(v);
            }
            for (int i = 1; i < 4; ++i) mesh.indices.insert(mesh.indices.end(), {base, base + i, base + i + 1});
        } else {
            add_polyline(mesh, pts, 5, color, 1.6f, true);
        }
        geometry_ = mesh ? GetRenderManager()->MakeGeometry(std::move(mesh)) : Rml::Geometry();
    }
    if (geometry_) geometry_.Render(GetAbsoluteOffset(Rml::BoxArea::Content));
}

} // namespace forge::ui
