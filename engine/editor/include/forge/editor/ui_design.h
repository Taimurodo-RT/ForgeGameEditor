#pragma once

// Game screens as the «Интерфейс» tab designs them: a tree of layers, the way
// Figma keeps a frame.
//
// A screen (the HUD, the main menu, a shop window) is a frame the size of the
// player's screen it was drawn for. Its layers are frames (which hold other
// layers), rectangles, ellipses, texts and pictures. Each layer has a place
// and size in its parent, constraints that say what it does when the parent
// changes size (stick to the left edge, to both edges, stay centred, scale),
// fills (colours, gradients, pictures, stacked), a stroke, corner radii,
// shadows and blurs, opacity and a blend mode. A frame can lay its children
// out itself (auto layout: in a row, a column or wrapping, with a gap, padding
// and one of nine alignments); its children then size to their content, to a
// fixed size or fill the free space.
//
// Drawn interfaces: a picture fill can repeat as a texture (its tile size and
// offset), a layer can wear a frame picture cut into nine parts whose corners
// keep their shape while the sides stretch or repeat (FrameArt, CSS
// border-image), and a picture can cut the layer's shape (MaskArt, CSS
// mask-image). The screen itself has settings: its background is the root
// frame's fills, the default font for its texts, how it fits the player's
// screen and a safe margin along the edges.
//
// On disk a screen is game/ui/<name>.json (save_screen, load_screen). What the
// game shows is the page screen_html() writes from it (game/ui/<name>.html):
// plain HTML and CSS, so everything the UI engine can draw from the web is
// available, and the page can be read and checked in a browser.
//
// Components (a button made once, used on every screen) live in the game's
// library, ui/components.json: see Node::component. A component's variants
// that name a state (Наведение, Нажата, Выключена) become the instance's
// look in that state on the page: a button drawn with pictures for each
// state just works.
//
// The snapping helpers at the end are the canvas's: they move a box so that
// its edges or middle line up with the screen's and other layers', and say
// which guide lines to draw.

#include "forge/core/types.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forge::editor::design {

struct Color {
    u8 r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Color&) const = default;
};
// "#rgb", "#rgba", "#rrggbb", "#rrggbbaa"; nullopt for anything else.
std::optional<Color> parse_color(std::string_view text);
// "#rrggbb", or "#rrggbbaa" when not opaque.
std::string color_hex(Color c);

struct GradientStop {
    Color color;
    f32 position = 0; // 0..1
    bool operator==(const GradientStop&) const = default;
};

enum class PaintKind : u8 { Solid, Linear, Radial, Image };
enum class ImageFit : u8 { Fill, Fit, Tile, Stretch }; // cover, contain, repeat, 100% 100%

// One fill of a layer (a layer may have several, drawn bottom to top).
struct Paint {
    PaintKind kind = PaintKind::Solid;
    Color color{255, 255, 255, 255};
    f32 angle = 180; // linear gradients, CSS degrees (180: top to bottom)
    std::vector<GradientStop> stops;
    std::string image; // a path relative to the game folder (pictures/...)
    ImageFit fit = ImageFit::Fill;
    f32 tile = 0;                 // Tile: one tile's width in pixels (0: the picture's own size)
    f32 offset_x = 0, offset_y = 0; // Tile: where the tiles start
    f32 opacity = 1;
    bool visible = true;
    std::string style; // Solid: the key of a colour of the game's (Screen::colors), followed when it changes
    bool operator==(const Paint&) const = default;
};

enum class StrokeAlign : u8 { Inside, Center, Outside };
enum class StrokeStyle : u8 { Solid, Dashed, Dotted };

struct Stroke {
    Color color{0, 0, 0, 255};
    f32 width = 1;
    StrokeAlign align = StrokeAlign::Inside;
    StrokeStyle style = StrokeStyle::Solid;
    bool visible = true;
    bool operator==(const Stroke&) const = default;
};

enum class EffectKind : u8 { DropShadow, InnerShadow, LayerBlur, BackgroundBlur };

struct Effect {
    EffectKind kind = EffectKind::DropShadow;
    Color color{0, 0, 0, 64};
    f32 x = 0, y = 4, blur = 4, spread = 0;
    bool visible = true;
    bool operator==(const Effect&) const = default;
};

// The blend modes of the UI engine (CSS mix-blend-mode).
enum class Blend : u8 {
    Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn, HardLight, SoftLight,
    Difference, Exclusion, Hue, Saturation, Color, Luminosity, PlusLighter
};
const char* blend_css(Blend b);

// A frame picture cut into nine parts: the corners keep their shape, the
// sides and the middle stretch or repeat. slice: how far in from each edge
// of the picture the cuts are (picture pixels: top, right, bottom, left);
// on the layer the parts are slice * scale wide.
enum class ArtRepeat : u8 { Stretch, Repeat, Round, Space };
struct FrameArt {
    std::string image; // empty: no frame picture
    std::array<f32, 4> slice{16, 16, 16, 16};
    f32 scale = 1;
    bool fill = true; // draw the middle part too
    ArtRepeat repeat = ArtRepeat::Stretch;
    bool visible = true;
    bool operator==(const FrameArt&) const = default;
};

// A picture whose opaque parts show the layer (with what is inside it).
struct MaskArt {
    std::string image; // empty: no mask
    ImageFit fit = ImageFit::Stretch;
    bool visible = true;
    bool operator==(const MaskArt&) const = default;
};

enum class TextAlign : u8 { Left, Center, Right, Justify };
enum class TextCase : u8 { None, Upper, Lower, Title };
enum class TextDecoration : u8 { None, Underline, Strike };

struct TextStyle {
    std::string family = "Onest"; // empty: the screen's font
    f32 size = 24;                // 0: the screen's size
    u16 weight = 400;
    bool italic = false;
    f32 line_height = 0;    // pixels; 0: the font's own
    f32 letter_spacing = 0; // pixels
    TextAlign align = TextAlign::Left;
    TextCase text_case = TextCase::None;
    TextDecoration decoration = TextDecoration::None;
    Color color{255, 255, 255, 255};
    std::string style; // the key of a text style of the game's (Screen::text_styles), followed when it changes
    bool operator==(const TextStyle&) const = default;
};

// What a layer does when its parent changes size, per axis.
enum class Constraint : u8 { Start, End, Both, Center, Scale };
// The size of a layer inside an auto layout, per axis.
enum class Sizing : u8 { Fixed, Hug, Fill };
enum class LayoutMode : u8 { None, Row, Column, Wrap };

struct AutoLayout {
    LayoutMode mode = LayoutMode::None;
    f32 gap = 0;
    std::array<f32, 4> padding{}; // top, right, bottom, left
    u8 align = 0;                 // 0..8: top left, top centre ... bottom right
    bool space_between = false;
    bool operator==(const AutoLayout&) const = default;
};

enum class NodeType : u8 { Frame, Rectangle, Ellipse, Text, Image };
const char* node_type_name(NodeType t); // "frame", "rectangle", ...

struct Node {
    u32 id = 0;
    std::string name;
    NodeType type = NodeType::Frame;
    bool visible = true;
    bool locked = false;

    // Place and size in the parent (pixels of the design). Inside an auto
    // layout x and y are where the layout put it last (not saved).
    f32 x = 0, y = 0, w = 100, h = 100;
    f32 rotation = 0; // degrees, clockwise, around the middle
    Constraint horizontal = Constraint::Start;
    Constraint vertical = Constraint::Start;
    Sizing width_sizing = Sizing::Fixed;
    Sizing height_sizing = Sizing::Fixed;
    bool absolute = false; // inside an auto layout but placed by x/y

    AutoLayout layout; // frames
    bool clip = false; // frames: hide what sticks out

    std::vector<Paint> fills;
    std::vector<Stroke> strokes;
    std::array<f32, 4> radius{}; // top left, top right, bottom right, bottom left
    f32 opacity = 1;
    Blend blend = Blend::Normal;
    std::vector<Effect> effects;
    FrameArt frame; // drawn over the fills
    MaskArt mask;

    std::string text; // texts
    TextStyle text_style;

    std::vector<Node> children; // frames, drawn first to last (last on top)

    // Components. In the game's library (a screen of its own, Screen::library)
    // every layer at the top is a variant of a component: component is the
    // component's name and variant its values ({"Состояние", "Наведение"}).
    // On a screen, a copy of a component (an instance) has component and
    // variant set at its top; it and every layer inside it keep the id of the
    // library layer they copy (master), and the fields the author changed
    // here (overrides: "text", "fills", ...) survive when the component
    // changes.
    std::string component;
    std::vector<std::pair<std::string, std::string>> variant;
    u32 master = 0;
    std::vector<std::string> overrides;

    bool is_container() const { return type == NodeType::Frame; }
};

// A guide line the author pulled out of a ruler.
struct Guide {
    bool vertical = true; // a vertical line at x = position
    f32 position = 0;
    bool operator==(const Guide&) const = default;
};

// key: what layers refer to it by (stays when it is renamed); name: what the author sees.
struct NamedColor {
    std::string key, name;
    Color color;
    bool operator==(const NamedColor&) const = default;
};
struct NamedTextStyle {
    std::string key, name;
    TextStyle style;
    bool operator==(const NamedTextStyle&) const = default;
};

// How a screen meets a player's screen of another size. Expand: scaled by the
// smaller side to keep its proportions, then grown to fill the rest (layers
// keep to their constraints). Fit: scaled whole, with bars along the sides.
// Stretch: scaled on each axis to fill it.
enum class ScreenFit : u8 { Expand, Fit, Stretch };

struct Screen {
    std::string title;          // shown in the list of screens
    f32 width = 1920, height = 1080; // the size it was drawn for
    Node root;                  // the screen itself (a frame; its fills are the background)
    std::vector<Guide> guides;
    u32 next_id = 1;
    // The screen's settings.
    TextStyle text{"Onest", 24, 400, false, 0, 0, {}, {}, {}, {255, 255, 255, 255}, {}}; // its texts' font, size and colour by default
    ScreenFit fit = ScreenFit::Expand;
    Color bars{0, 0, 0, 255}; // Fit: the bars' colour
    f32 safe = 0;             // the safe margin along every edge (pixels): what matters stays inside
    bool library = false;     // the game's components (ui/components.json), not a screen the game shows
    // The library's game styles: named colours and text styles that layers on
    // every screen can use (Paint::style, TextStyle::style); changing one
    // changes them all.
    std::vector<NamedColor> colors;
    std::vector<NamedTextStyle> text_styles;
};

// A new screen with an empty root frame.
Screen make_screen(std::string title, f32 width, f32 height);

std::string save_screen(const Screen& screen);
bool load_screen(std::string_view json, Screen& out, std::string* error = nullptr);

// --- the tree ---
Node* find(Node& root, u32 id);
const Node* find(const Node& root, u32 id);
// The parent of the node with id (nullptr for the root or an unknown id).
Node* parent_of(Node& root, u32 id);
// Ids from the root down to id (empty when not found).
std::vector<u32> path_to(const Node& root, u32 id);
// Takes a node out of the tree (with its children); nullopt when not found.
std::optional<Node> remove(Node& root, u32 id);
// Gives node and its children fresh ids from screen.next_id.
void renumber(Screen& screen, Node& node);
// "Прямоугольник 3": the type's word and the next free number on the screen.
std::string fresh_name(const Screen& screen, NodeType type);

// --- components ---
// One component of the library: its variants (library layer ids, in order)
// and its properties with the values the variants use.
struct ComponentProperty {
    std::string name;
    std::vector<std::string> values;
};
struct Component {
    std::string name;
    std::vector<u32> variants;
    std::vector<ComponentProperty> properties;
};
std::vector<Component> components(const Screen& library);
const Component* find_component(const std::vector<Component>& list, std::string_view name);
// The library layer for these values: the variant matching most of them
// (the first variant when none match); nullptr for an unknown component.
const Node* find_variant(const Screen& library, std::string_view component,
                         const std::vector<std::pair<std::string, std::string>>& values);
// The value of a property in a variant list ("" when missing).
std::string variant_value(const std::vector<std::pair<std::string, std::string>>& values, std::string_view property);
void set_variant_value(std::vector<std::pair<std::string, std::string>>& values, const std::string& property,
                       const std::string& value);
// A copy of a component for screen (fresh ids from screen.next_id); nullopt
// for an unknown component.
std::optional<Node> make_instance(Screen& screen, const Screen& library, std::string_view component,
                                  const std::vector<std::pair<std::string, std::string>>& values = {});
// Brings an instance up to date with its component (its variant's layers,
// with the author's overrides kept); true when anything changed.
bool sync_instance(Screen& screen, Node& instance, const Screen& library);
// Every instance on the screen; true when anything changed.
bool sync_instances(Screen& screen, const Screen& library);
// Gives the layers that use the library's colours and text styles their
// current values; true when anything changed.
bool apply_styles(Screen& screen, const Screen& library);

// The override a design panel field belongs to ("fill.0.color" -> "fills",
// "w" -> "size"); empty for fields that are not kept (place, name).
std::string override_of(std::string_view field);
// Turns an instance back into plain layers.
void detach(Node& instance);
// The top of the instance holding id (the instance itself for its top);
// nullptr when id is not in an instance.
const Node* instance_of(const Node& root, u32 id);
// The states of a button that a variant property can name, with the page's
// selector for them: "Наведение" -> ":hover", "Нажата" -> ":active",
// "Выключена" -> ".disabled", "Выбрана" -> ".selected", "Фокус" -> ":focus".
// Empty for anything else (the usual look).
std::string state_selector(std::string_view value);
// The property of a component that switches states (the one whose values
// name states), or "".
std::string state_property(const Component& component, const Screen& library);

// --- the page the game shows ---
struct HtmlOptions {
    // Stylesheets linked before the screen's own styles (fonts, a game theme).
    std::vector<std::string> stylesheets;
    // The game's components: instances whose component has states (hover,
    // pressed...) get the other variants' looks for them.
    const Screen* library = nullptr;
};
std::string screen_html(const Screen& screen, const HtmlOptions& options = {});
// The CSS of one layer (what the page's <style> holds for it), for tests and
// the code view: "#n12 { ... }". Every layer is an element with id "n<id>".
std::string node_css(const Node& node, const Node* parent, f32 parent_w, f32 parent_h);

// --- snapping on the canvas ---
struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;
    f32 right() const { return x + w; }
    f32 bottom() const { return y + h; }
    f32 cx() const { return x + w * 0.5f; }
    f32 cy() const { return y + h * 0.5f; }
};

// A line drawn while snapping: on x (vertical) or y (horizontal), spanning
// from..to along the other axis.
struct SnapLine {
    bool vertical = true;
    f32 at = 0;
    f32 from = 0, to = 0;
};
// The gap between two boxes when equal spacing snapped, with its length.
struct SnapGap {
    bool horizontal = true; // measured along x
    f32 from = 0, to = 0;   // along the measured axis
    f32 at = 0;             // across it (where to draw)
};

struct SnapResult {
    f32 dx = 0, dy = 0; // add to the moving box
    bool snapped_x = false, snapped_y = false;
    std::vector<SnapLine> lines;
    std::vector<SnapGap> gaps;
};

struct SnapTargets {
    std::vector<Rect> boxes;     // other layers (in the same coordinates)
    std::vector<f32> xs, ys;     // guide lines
    Rect frame{};                // the screen: its edges and middle
    f32 grid = 0;                // > 0: snap to a grid when nothing else is near
};

// Moves box to the nearest edge or middle within threshold on each axis
// (left, middle and right edges against the targets' left, middle, right),
// and to equal gaps between neighbours.
SnapResult snap_box(const Rect& box, const SnapTargets& targets, f32 threshold);
// Resizing: only the edges being dragged snap (left/right/top/bottom true).
SnapResult snap_edges(const Rect& box, bool left, bool right, bool top, bool bottom, const SnapTargets& targets,
                      f32 threshold);

} // namespace forge::editor::design
