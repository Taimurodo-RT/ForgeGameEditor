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
#include <utility>
#include <vector>

namespace forge::pixel {

// What of a block a mouse edit changes: the inspector's fields and the timeline's edges.
enum class BlockEdit : u8 { Value, Length, Timing, Start };
// Whether that of block b can be edited: false with why, the words its edit would refuse with (a binding, a spring,
// tween's frames, back, restore, rest, a module's call, a block without a start of its own).
bool can_edit(const Recording& rec, const Block& b, BlockEdit what, const CodeSites& code, std::string& why);
bool can_edit_pause(const Recording& rec, const Pause& pause, const CodeSites& code, std::string& why);

// The value block b goes to, of property p: its own, or another one added to the table of its call
// (part("body"):to({ y = 2 }, …) → { y = 2, x = 3 }). A call of one property (body.y(…)) takes no other: add_at_line.
// An own value (nil) goes into no table: { fill = nil } is a table without fill, refused (part 4ж).
bool edit_value(const Recording& rec, const Block& b, Prop p, const PropValue& value, const CodeSites& code,
                std::string_view source, std::string& out, std::string& why);
// Its length: the number in its call, added when the call has none.
bool edit_length(const Recording& rec, const Block& b, f64 length, const CodeSites& code, std::string_view source,
                 std::string& out, std::string& why);
// Its timing, as a word: written into its call, added when the call has none.
bool edit_timing(const Recording& rec, const Block& b, vector::Timing timing, const CodeSites& code,
                 std::string_view source, std::string& out, std::string& why);
// Its timing as bezier(x1, y1, x2, y2), in the same place (part 4ж): body.y(2, 0.4, "out") → body.y(2, 0.4,
// bezier(0.25, 0.1, 0.25, 1)).
bool edit_timing_bezier(const Recording& rec, const Block& b, const vector::Bezier& curve, const CodeSites& code,
                        std::string_view source, std::string& out, std::string& why);
// The numbers of the bezier(…) call that made block b's easing, those that changed (part 4ж); false with why for a
// block without one, one of a module's call, numbers out of their bounds.
bool can_edit_bezier(const Recording& rec, const Block& b, const CodeSites& code, std::string& why);
bool edit_bezier(const Recording& rec, const Block& b, const vector::Bezier& curve, const CodeSites& code,
                 std::string_view source, std::string& out, std::string& why);
// The blocks whose easing that call made: it, or a variable or a loop holding it (part 4ж).
usize bezier_users(const Recording& rec, const Block& b);
// Its start, when it starts at an at(с, …) or a delay(с, …): their number.
bool edit_start(const Recording& rec, const Block& b, f64 start, const CodeSites& code, std::string_view source,
                std::string& out, std::string& why);
// An actor's clip in a scene (part 6а of docs/models/сцены-модель.md): its start, as a block's (an at(с, …) or a
// delay(с, …) it starts at); its length is its character's clip's and is not edited.
bool can_edit_play_start(const Recording& rec, const ActorPlay& play, const CodeSites& code, std::string& why);
bool edit_play_start(const Recording& rec, const ActorPlay& play, f64 start, const CodeSites& code, std::string_view source,
                     std::string& out, std::string& why);
// A pause's length: wait(с), t:wait(с).
bool edit_pause(const Recording& rec, const Pause& pause, f64 length, const CodeSites& code, std::string_view source,
                std::string& out, std::string& why);
// A new first line of clip rec.clip, for a part moved where no transition of it runs:
// at(0.25, part("head").rot(5, 0.1)), or at(0.25, part("mouth"):morph("mouth_open", 0.1)).
bool add_at_line(const Recording& rec, f64 t, std::string_view part, Prop p, const PropValue& value, f64 length,
                 const CodeSites& code, std::string_view source, std::string& out, std::string& why);
// The same with numbers of a part in one table: at(0.25, part("head"):to({ x = 3, y = -2 }, 0)).
bool add_at_table(const Recording& rec, f64 t, std::string_view part, const std::vector<std::pair<Prop, f64>>& values,
                  f64 length, const CodeSites& code, std::string_view source, std::string& out, std::string& why);

// A part dragged in the view in the frame at t (`frame` seconds long) by dx, dy, in the units its x and y move it by
// («Правки мышью (часть 3б)»). For each of x and y that moved: the deciding block's end value plus the move when its
// transition goes in this frame; else a field of the other one's call when that one goes, its table has no such field
// and this track started nothing since; else, both of them in one new first line of the clip,
// at(t, part("head"):to({ x = …, y = … }, 0)), at t to thousandths not after it, from the recording's values at t.
// Every change in one text; false with why.
bool edit_part_move(const Recording& rec, std::string_view part, f64 t, f64 frame, f64 dx, f64 dy, const CodeSites& code,
                    std::string_view source, std::string& out, std::string& why);

// A part's field (part 4ж): what the clip's code gives property p of the part in the frame at t, `frame` seconds long —
// its deciding block's end when that one's transition goes in this frame (going true), else the recording's value
// there. False when a binding decides: its function gives the value, the code no end (the field shows the frame's).
bool part_prop_value(const Recording& rec, std::string_view part, f64 t, f64 frame, Prop p, PropValue& out, bool& going);
// Property p of the part made `value` in that frame: the deciding block's end when its transition goes; else a field
// of the table of a call of another property of the part going there that has no such field, when p's track started
// nothing since (of several, the one that started last; together, the first by the list of properties); else a new
// first line of the clip, at(t, part("head").rot(10, 0)), at t to thousandths not after it. An own value (nil) goes
// into no table: a table with field = nil has no field.
bool edit_part_prop(const Recording& rec, std::string_view part, f64 t, f64 frame, Prop p, const PropValue& value,
                    const CodeSites& code, std::string_view source, std::string& out, std::string& why);

} // namespace forge::pixel
