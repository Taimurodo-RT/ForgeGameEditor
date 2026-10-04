#include "elements.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/ui/virtual_list.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace forge::ui {

namespace {

std::unordered_map<std::string, u32> g_icons;
std::unordered_map<std::string, ListSource*> g_sources;
std::unordered_set<ElementVirtualList*> g_lists;

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
    if (content_) {
        content_->RemoveEventListener("click", this);
        content_->RemoveEventListener("dblclick", this);
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
    content_->AddEventListener("click", this);
    content_->AddEventListener("dblclick", this);
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
            count_ = ~0u;
            for (Row& r : rows_) r.index = ~0u, r.element->SetProperty("display", "none");
        }
        break;
    }

    const float client = std::max(GetClientHeight(), row_height_);
    const u32 needed = static_cast<u32>(std::ceil(client / row_height_)) + 1;
    while (rows_.size() < needed) {
        Row row;
        Rml::ElementPtr element = GetOwnerDocument()->CreateElement("div");
        row.element = content_->AppendChild(std::move(element));
        row.element->SetClass("vl-row", true);
        row.element->SetProperty("position", "absolute");
        row.element->SetProperty("left", "0px");
        row.element->SetProperty("right", "0px");
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
        const float top = static_cast<float>(std::min<i64>(pending_scroll_row_, count - 1)) * row_height_;
        if (top < GetScrollTop()) SetScrollTop(top);
        else if (top + row_height_ > GetScrollTop() + GetClientHeight())
            SetScrollTop(top + row_height_ - GetClientHeight());
        pending_scroll_row_ = -1;
    }

    u32 first = static_cast<u32>(std::max(0.0f, GetScrollTop()) / row_height_);
    if (count == 0) first = 0;
    else if (first >= count) first = count - 1;

    const bool layout_changed = first != first_ || count != count_;
    const bool data_changed = version != version_;
    if (!layout_changed && !data_changed) return;

    if (count != count_) content_->SetProperty("height", std::to_string(static_cast<double>(count) * row_height_) + "px");

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
        row.element->SetProperty("top", std::to_string(static_cast<double>(index) * row_height_) + "px");
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
    while (e && e->GetParentNode() != content_) e = e->GetParentNode();
    if (!e) return;
    for (const Row& row : rows_) {
        if (row.element != e || row.index == ~0u) continue;
        int modifiers = 0;
        if (event.GetParameter<int>("ctrl_key", 0)) modifiers |= Rml::Input::KM_CTRL;
        if (event.GetParameter<int>("shift_key", 0)) modifiers |= Rml::Input::KM_SHIFT;
        if (event.GetParameter<int>("alt_key", 0)) modifiers |= Rml::Input::KM_ALT;
        source->on_row_event(row.index, event.GetType(), modifiers);
        break;
    }
}

} // namespace forge::ui
