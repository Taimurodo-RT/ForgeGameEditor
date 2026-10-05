#pragma once

// Custom elements: <icon>, <virtual-list>, <lines> and <shape>.

#include "forge/core/types.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Geometry.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::ui {

// Material Symbols names ("save", "play_arrow") to codepoints, from the
// icons.codepoints file shipped with the font.
bool load_icon_codepoints(const std::filesystem::path& file);
u32 icon_codepoint(std::string_view name);

// <icon name="save"/>: one glyph of the icon font, chosen by name. The font
// and size come from the style sheet like any text.
class ElementIcon final : public Rml::Element {
public:
    explicit ElementIcon(const Rml::String& tag_name);

protected:
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
};

class ListSource;

class ElementVirtualList final : public Rml::Element, public Rml::EventListener {
public:
    explicit ElementVirtualList(const Rml::String& tag_name);
    ~ElementVirtualList() override;

    void scroll_to_row(u32 row);
    const std::string& source_name() const { return source_name_; }

protected:
    void OnUpdate() override;
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
    void ProcessEvent(Rml::Event& event) override;

private:
    struct Binding {
        Rml::Element* element;
        enum Kind : u8 { Text, Attribute, Class, Style } kind;
        std::string target; // attribute, class or property name
        std::string field;
        std::string value;  // last value set
        bool set = false;
    };
    struct Row {
        Rml::Element* element = nullptr;
        std::vector<Binding> bindings;
        u32 index = ~0u;
    };

    void build();
    void bind_row(Row& row, u32 index, ListSource& source, bool force);

    std::string source_name_;
    std::string template_rml_;
    float row_height_ = 28.0f;
    float cell_width_ = 0; // > 0: a grid
    u32 columns_ = 1;
    float cell_w_ = 0;
    void clear_rows();
    bool built_ = false;
    Rml::Element* content_ = nullptr;
    std::vector<Row> rows_;
    u32 first_ = ~0u;
    u32 count_ = ~0u;
    u64 version_ = ~0ull;
    i64 pending_scroll_row_ = -1;
};

// <lines source="…">: see forge/ui/drawing.h.
class ElementLines final : public Rml::Element {
public:
    explicit ElementLines(const Rml::String& tag_name);

protected:
    void OnRender() override;
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;

private:
    std::string source_name_;
    u64 version_ = ~0ull;
    Rml::Geometry geometry_;
};

// <shape kind="exec"/>: see forge/ui/drawing.h.
class ElementShape final : public Rml::Element {
public:
    explicit ElementShape(const Rml::String& tag_name);

protected:
    void OnRender() override;
    void OnResize() override;
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
    void OnPropertyChange(const Rml::PropertyIdSet& changed) override;

private:
    Rml::Geometry geometry_;
    bool dirty_ = true;
    bool filled_ = false;
};

} // namespace forge::ui
