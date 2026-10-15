#pragma once

// A mouse edit of a recorded clip, written into its code (step 1 of docs/models/движение-модель.md, «Как правка мышью
// переписывает код»): a block knows the call that made it and which of the call's arguments hold its value, its
// length and its timing, so only their characters change (code_sites.h). A block of a module the script requires is
// not edited: the change would move every character that uses the module.
//
//   edit_length(rec, rec.blocks[3], 0.4, script.code(), text, out, why);   // body.y(2, 0.1) → body.y(2, 0.4)
//
// Each takes the text the script's code was parsed from (PoseScript::code()) and gives the whole new text in out, or
// false with why (Russian).

#include "forge/pixel/code_sites.h"
#include "forge/pixel/motion.h"

#include <string>
#include <string_view>

namespace forge::pixel {

// The value block b goes to, of property p: its own, or another one added to the table of its call
// (part("body"):to({ y = 2 }, …) → { y = 2, x = 3 }). A call of one property (body.y(…)) takes no other: add_at_line.
bool edit_value(const Recording& rec, const Block& b, Prop p, const PropValue& value, const CodeSites& code,
                std::string_view source, std::string& out, std::string& why);
// Its length: the number in its call, added when the call has none.
bool edit_length(const Recording& rec, const Block& b, f64 length, const CodeSites& code, std::string_view source,
                 std::string& out, std::string& why);
// Its timing, as a word: written into its call, added when the call has none.
bool edit_timing(const Recording& rec, const Block& b, vector::Timing timing, const CodeSites& code,
                 std::string_view source, std::string& out, std::string& why);
// Its start, when it starts at an at(с, …) or a delay(с, …): their number.
bool edit_start(const Recording& rec, const Block& b, f64 start, const CodeSites& code, std::string_view source,
                std::string& out, std::string& why);
// A pause's length: wait(с), t:wait(с).
bool edit_pause(const Recording& rec, const Pause& pause, f64 length, const CodeSites& code, std::string_view source,
                std::string& out, std::string& why);
// A new first line of clip rec.clip, for a part moved where no transition of it runs:
// at(0.25, part("head").rot(5, 0.1)), or at(0.25, part("mouth"):morph("mouth_open", 0.1)).
bool add_at_line(const Recording& rec, f64 t, std::string_view part, Prop p, const PropValue& value, f64 length,
                 const CodeSites& code, std::string_view source, std::string& out, std::string& why);

} // namespace forge::pixel
