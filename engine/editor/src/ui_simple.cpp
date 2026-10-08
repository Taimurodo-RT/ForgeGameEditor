#include "forge/editor/ui_simple.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace forge::editor::design {

namespace {

constexpr const char* kBlockKeys[] = {"screen", "button", "text", "picture", "bar", "list", "group", "shape"};
constexpr const char* kBlockWords[] = {"Экран", "Кнопка", "Текст", "Картинка", "Полоска", "Список", "Группа", "Фигура"};

bool shows_picture(const Node& n) {
    if (!n.children.empty()) return false;
    for (const Paint& p : n.fills)
        if (p.visible && p.kind == PaintKind::Image) return true;
    return false;
}

Paint solid(Color c) {
    Paint p;
    p.color = c;
    return p;
}

Node text_node(Screen& s, std::string name, std::string text, f32 size) {
    Node t;
    t.id = s.next_id++;
    t.type = NodeType::Text;
    t.name = std::move(name);
    t.text = std::move(text);
    t.width_sizing = Sizing::Hug;
    t.height_sizing = Sizing::Hug;
    t.w = 200;
    t.h = size * 1.3f;
    t.text_style.family.clear(); // the screen's font
    t.text_style.size = size;
    t.text_style.color = s.text.color;
    return t;
}

} // namespace

// "Кнопка 3": the block's word and the next number no layer of the screen has.
std::string fresh_block_name(const Screen& s, const char* word) {
    int top = 0;
    const std::string prefix = std::string(word) + " ";
    auto visit = [&](auto&& self, const Node& n) -> void {
        if (n.name.rfind(prefix, 0) == 0) top = std::max(top, std::atoi(n.name.c_str() + prefix.size()));
        for (const Node& c : n.children) self(self, c);
    };
    visit(visit, s.root);
    return prefix + std::to_string(top + 1);
}

const char* block_word(Block b) { return kBlockWords[static_cast<int>(b)]; }
const char* block_key(Block b) { return kBlockKeys[static_cast<int>(b)]; }

std::optional<Block> parse_block(std::string_view key) {
    for (int i = 0; i < static_cast<int>(std::size(kBlockKeys)); ++i)
        if (key == kBlockKeys[i]) return static_cast<Block>(i);
    return std::nullopt;
}

Block block_of(const Node& n, bool root) {
    if (root) return Block::Screen;
    if (n.list != ListSource::None) return Block::List;
    if (!n.bar.value.empty()) return Block::Bar;
    if (n.type == NodeType::Text) return Block::Text;
    if (n.type == NodeType::Image || shows_picture(n)) return Block::Picture;
    if (n.is_container() && !n.children.empty())
        return !n.on_click.empty() || !n.component.empty() || (n.children.size() == 1 && n.children[0].type == NodeType::Text)
                   ? Block::Button
                   : Block::Group;
    if (!n.on_click.empty()) return Block::Button;
    return Block::Shape;
}

const Node* block_label(const Node& n) {
    for (const Node& c : n.children) {
        if (c.type == NodeType::Text) return &c;
        if (const Node* deeper = block_label(c)) return deeper;
    }
    return nullptr;
}

Node* block_label(Node& n) { return const_cast<Node*>(block_label(static_cast<const Node&>(n))); }

bool simple_action(ActionKind k) {
    switch (k) {
    case ActionKind::Show:
    case ActionKind::Close:
    case ActionKind::Message:
    case ActionKind::NewGame:
    case ActionKind::Continue:
    case ActionKind::Load:
    case ActionKind::Save:
    case ActionKind::Settings:
    case ActionKind::Pause:
    case ActionKind::Resume:
    case ActionKind::Menu:
    case ActionKind::Quit: return true;
    default: return false; // hide, toggle, change, talk: «Полный»
    }
}

bool simple_action_editable(const Node& n) { return n.on_click.empty() || (n.on_click.size() == 1 && simple_action(n.on_click[0].kind)); }

std::vector<std::string> simple_hidden(const Node& n, bool root) {
    std::vector<std::string> out;
    const Block b = block_of(n, root);
    auto add = [&](const char* what) {
        if (std::find(out.begin(), out.end(), what) == out.end()) out.emplace_back(what);
    };
    if (!n.component.empty() && n.master) add("копия компонента: её вид задаёт компонент");
    if (n.rotation != 0) add("поворот");
    if (n.opacity < 1) add("прозрачность");
    if (n.blend != Blend::Normal) add("наложение");
    if (!n.effects.empty()) add("тени и размытие");
    if (!n.strokes.empty()) add("обводка");
    if (!n.frame.image.empty()) add("рисованная рамка");
    if (!n.mask.image.empty()) add("маска");
    // The simple panel shows one solid colour (a picture's one picture).
    usize shown = 0;
    bool plain = true;
    for (const Paint& p : n.fills) {
        ++shown;
        if (b == Block::Picture ? p.kind != PaintKind::Image : p.kind != PaintKind::Solid) plain = false;
        if (p.opacity < 1 || !p.visible) plain = false;
    }
    if (b == Block::Text ? shown > 0 : (shown > 1 || !plain)) add("сложная заливка");
    if (n.motion.kind != MotionKind::None) add("движение");
    if (n.smooth > 0) add("плавная смена вида");
    if (!n.show_if.empty()) add("условие показа");
    if (!simple_action_editable(n)) add("несколько или особые действия при нажатии");
    if (n.layout.mode != LayoutMode::None && b != Block::Button) add("автораскладка");
    if (b == Block::Bar && n.bar.from != BarFrom::Left) add("направление полоски");
    if (n.type == NodeType::Text) {
        const TextStyle& t = n.text_style;
        if (t.italic || t.line_height != 0 || t.letter_spacing != 0 || t.text_case != TextCase::None ||
            t.decoration != TextDecoration::None)
            add("оформление текста");
    }
    if (b == Block::Button)
        if (const Node* label = block_label(n); label && (!label->show_if.empty() || label->motion.kind != MotionKind::None))
            add("условие или движение надписи");
    return out;
}

Node make_block(Screen& s, Block b, f32 x, f32 y, const BlockOptions& o) {
    // The game's first colour paints what stands out (followed when it changes).
    Paint accent = solid(Color{0x2a, 0x20, 0x18, 240});
    if (o.library && !o.library->colors.empty()) {
        accent = solid(o.library->colors.front().color);
        accent.style = o.library->colors.front().key;
    }
    Node n;
    n.id = s.next_id++;
    n.x = std::round(x);
    n.y = std::round(y);
    n.name = fresh_block_name(s, block_word(b));
    switch (b) {
    case Block::Button: {
        n.type = NodeType::Frame;
        n.w = 320;
        n.h = 80;
        n.fills.push_back(accent);
        n.radius = {14, 14, 14, 14};
        n.layout.mode = LayoutMode::Row;
        n.layout.align = 4; // in the middle
        n.children.push_back(text_node(s, "Надпись", "Кнопка", 32));
        n.children.back().text_style.weight = 700;
        break;
    }
    case Block::Text: {
        Node t = text_node(s, n.name, "Текст", 32);
        t.id = n.id;
        t.x = n.x;
        t.y = n.y;
        return t;
    }
    case Block::Picture: {
        n.type = NodeType::Image;
        n.w = n.h = 160;
        Paint p;
        p.kind = PaintKind::Image;
        p.fit = ImageFit::Fit;
        p.image = o.picture;
        n.fills.push_back(p);
        break;
    }
    case Block::Bar:
        n.type = NodeType::Rectangle;
        n.w = 400;
        n.h = 32;
        n.radius = {8, 8, 8, 8};
        n.fills.push_back(o.library && !o.library->colors.empty() ? accent : solid(Color{0xd9, 0x4a, 0x3a, 255}));
        n.bar.value = o.bar_value;
        n.bar.max = o.bar_max;
        break;
    case Block::List: {
        n.type = NodeType::Frame;
        n.list = ListSource::Items;
        n.list_gap = 8;
        n.w = 616;
        n.h = 360;
        n.radius = {12, 12, 12, 12};
        n.fills.push_back(solid(Color{0, 0, 0, 0x80}));
        // The cell: the thing's picture and name with its count.
        Node cell;
        cell.id = s.next_id++;
        cell.name = "Ячейка";
        cell.type = NodeType::Frame;
        cell.x = cell.y = 8;
        cell.w = 296;
        cell.h = 80;
        cell.radius = {10, 10, 10, 10};
        cell.fills.push_back(accent);
        cell.layout.mode = LayoutMode::Row;
        cell.layout.align = 3; // middle left
        cell.layout.padding = {0, 16, 0, 16};
        cell.children.push_back(text_node(s, "Предмет", "{item.name} {item.count}", 24));
        cell.children.back().picture_from = "item.icon";
        n.children.push_back(std::move(cell));
        Node empty = text_node(s, "Пока пусто", "Пока ничего нет", 20);
        empty.x = empty.y = 8;
        n.children.push_back(std::move(empty));
        break;
    }
    case Block::Group:
        n.type = NodeType::Frame;
        n.w = 400;
        n.h = 300;
        break;
    case Block::Shape:
        n.type = NodeType::Rectangle;
        n.w = n.h = 160;
        n.fills.push_back(solid(Color{0xd9, 0xd9, 0xd9, 255}));
        break;
    case Block::Screen: break;
    }
    return n;
}

} // namespace forge::editor::design
