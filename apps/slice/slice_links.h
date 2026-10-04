#pragma once

// «Связи» over the running game (F2 when the editor started it): the things
// on screen get their names, the links between them arrows with their verbs,
// and a click on one thing, then on another, offers what the first can do
// with the second, the way the editor's board does. The game adds or removes
// the link in its logic.json and the editor, watching the file, takes it in.

#include "forge/logic/logic.h"
#include "forge/render/camera.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <functional>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
}

namespace slice {

using forge::f64;
using forge::u32;
using forge::usize;

// A thing on screen: the copy nearest to the hero (tiles), and how far its
// edge is from its middle.
struct Seen {
    std::string id; // a template id or logic::kHero
    f64 x = 0, y = 0;
    f64 half_w = 0.5, half_h = 0.5;
};

class LinkOverlay {
public:
    struct MarkView {
        Rml::String id, name;
        float x = 0, y = 0;        // the middle of the thing (px)
        float w = 0, h = 0;        // the ring around it (the name above it)
        bool picked = false, hero = false, linked = false;
        bool operator==(const MarkView&) const = default;
    };
    struct ArrowView {
        int id = 0;
        Rml::String verb, icon, phrase;
        float lx = 0, ly = 0, len = 0, angle = 0, hx = 0, hy = 0, cx = 0, cy = 0;
        bool lit = false;
        bool operator==(const ArrowView&) const = default;
    };
    struct RowView {
        int id = 0;
        Rml::String phrase, icon;
        bool lit = false, here = false; // here: both things are on screen
        bool operator==(const RowView&) const = default;
    };
    struct ChoiceView {
        Rml::String phrase, about, icon;
        bool operator==(const ChoiceView&) const = default;
    };

    void bind(Rml::DataModelConstructor& c);
    void set_handle(Rml::DataModelHandle h) { handle_ = h; }

    // What a choice means to the game: a new link (with its usual
    // refinements), a link taken away.
    std::function<void(const forge::logic::Link&)> on_add;
    std::function<void(u32)> on_remove;

    // Only a game the editor started can change its links.
    void set_allowed(bool allowed) { allowed_ = allowed; }
    bool allowed() const { return allowed_; }
    bool on() const { return on_; }
    void show(bool on);

    // Each frame while shown: places the marks and arrows. lit: whether a link
    // has just happened.
    void update(const std::vector<Seen>& seen, const forge::render::Camera2D& camera, u32 width, u32 height,
                const forge::logic::Logic& logic, const forge::logic::Verbs& verbs,
                const std::vector<forge::logic::Thing>& things, const std::function<bool(u32)>& lit);

    // A click on a thing's mark: the first, then the second (the choices).
    void click(const std::string& id);
    // A click on the world, not on a mark: nothing picked any more.
    void cancel();
    bool picking() const { return !pick_b_.empty(); }
    const std::string& picked() const { return pick_a_; }
    const std::vector<ChoiceView>& choices() const { return m_choices_; }
    bool choose(usize i);
    const std::vector<RowView>& rows() const { return m_rows_; }
    const std::vector<MarkView>& marks() const { return m_marks_; }

private:
    struct Option {
        std::string a, verb, b;
    };
    template <class T>
    void set(T& member, const T& value, const char* name) {
        if (member == value) return;
        member = value;
        if (handle_) handle_.DirtyVariable(name);
    }
    void open_choices();

    Rml::DataModelHandle handle_;
    bool allowed_ = false, on_ = false;
    std::string pick_a_, pick_b_;
    std::vector<Option> options_;
    const forge::logic::Verbs* verbs_ = nullptr;
    const std::vector<forge::logic::Thing>* things_ = nullptr;
    u32 width_ = 0, height_ = 0;

    // Model mirrors
    bool m_on_ = false, m_allowed_ = false, m_picking_ = false, m_first_ = false;
    std::vector<MarkView> m_marks_;
    std::vector<ArrowView> m_arrows_;
    std::vector<RowView> m_rows_;
    std::vector<ChoiceView> m_choices_;
    Rml::String m_title_, m_tip_;
    float m_pick_x_ = 0, m_pick_y_ = 0;
};

} // namespace slice
