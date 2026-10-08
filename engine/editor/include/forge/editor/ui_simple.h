#pragma once

// «Простой» mode of the «Интерфейс» tab: the same screens, seen as a few
// blocks a beginner knows (a button, a text, a picture, a bar, a list) with
// a few plain properties each. It is only a way of looking at a screen: the
// layers stay what they are, and everything the simple panel does not show
// (shadows, movement, auto layout, several actions...) is kept as it is and
// named, so the author knows to look in «Полный».
//
// block_of() says what block a layer reads as, simple_hidden() what it has
// beyond its block's simple properties, make_block() builds a new block out
// of ordinary layers (frames, texts, pictures; the game's first colour when
// the library has one), and simple_action_editable() whether its clicks fit
// the simple panel's one action.

#include "forge/editor/ui_design.h"

#include <string>
#include <vector>

namespace forge::editor::design {

enum class Block : u8 {
    Screen,  // the screen itself: its title, background, when the game shows it
    Button,  // something to press: a frame with a label, or anything with an action
    Text,    // a text
    Picture, // a picture
    Bar,     // a bar whose length follows the game's data
    List,    // a list of the hero's things or quests
    Group,   // a frame holding other layers
    Shape,   // a rectangle or an ellipse
};
// What the layer reads as (root: the screen's own root frame).
Block block_of(const Node& node, bool root = false);
const char* block_word(Block b); // "Кнопка", "Текст"...
const char* block_key(Block b);  // "button", "text"... (the panel and the page's ids)
std::optional<Block> parse_block(std::string_view key);
// "Кнопка 3": the word and the next number no layer of the screen has.
std::string fresh_block_name(const Screen& screen, const char* word);

// A button's label: its first text, depth first (nullptr: none).
const Node* block_label(const Node& node);
Node* block_label(Node& node);

// The actions a simple button offers: one each, with a screen name for
// «show», a name for «message», nothing for the rest.
bool simple_action(ActionKind kind);
// Its clicks fit the simple panel: none, or one simple action.
bool simple_action_editable(const Node& node);

// What the layer has that its block's simple properties do not show, in
// the author's words («тени и размытие», «движение»...); empty: nothing.
std::vector<std::string> simple_hidden(const Node& node, bool root = false);

// A new block at (x, y) of its parent, built from ordinary layers with fresh
// ids from screen.next_id. library: the game's colours (the first one paints
// buttons and bars); picture: the picture a picture block shows; bar: the
// game's value it follows and its maximum.
struct BlockOptions {
    const Screen* library = nullptr;
    std::string picture;
    std::string bar_value = "hero.hearts", bar_max = "hero.hearts_max";
};
Node make_block(Screen& screen, Block b, f32 x, f32 y, const BlockOptions& options = {});

} // namespace forge::editor::design
