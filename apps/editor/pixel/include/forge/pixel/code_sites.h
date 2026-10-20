#pragma once

// Where each call of a character's code is («Векторная», step 1 of docs/models/движение-модель.md). The Luau parser
// finds every call with its arguments; the code is run instrumented, so the recording knows which call made each
// block; a mouse edit rewrites only the characters of one argument and leaves the rest of the file byte for byte.
//
//   CodeSites code;
//   code.parse(text, why, line);              // false: the code does not parse, why and its line
//   luau_compile(code.instrumented(0))        // f(a) → __fc(0, f, a), o:m(a) → __fm(1, o, "m", a); same lines
//   rewrite_number(text, code.sites()[1].args[0], 2, 3.5, out)   // body.y(2, 0.1) → body.y(3.5, 0.1)

#include "forge/core/types.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forge::pixel {

// Bytes of the source: [begin, end).
struct CodeSpan {
    u32 begin = 0, end = 0;
    bool operator==(const CodeSpan&) const = default;
};

enum class ArgKind : u8 {
    Number,   // 2, -0.5, 0x10
    String,   // "out_quad"
    Table,    // {y = 2, sy = 0.9}
    Function, // function(k) return k * k end
    Other,    // any other expression: -p.height, 28 * s, part("head")
};

struct CodeField;

struct CodeArg {
    ArgKind kind = ArgKind::Other;
    CodeSpan span;
    f64 number = 0;   // Number: with its minus, "-2" is −2
    std::string text; // String: its value

    // Table: its named fields (x = 1, ["x"] = 1) in order, how many items it has in all, where the last one starts
    // and ends (both past "{" when there is none), the separator after it (-1 when none) and the closing "}".
    std::vector<CodeField> fields;
    u32 items = 0;
    u32 last_begin = 0, last_end = 0;
    i32 last_sep = -1;
    u32 close = 0;

    // Other: "e + n" or "e - n" at its top, n a number: where the sign and the number are.
    bool tail = false, tail_minus = false;
    u32 tail_op = 0;
    CodeSpan tail_span;
    f64 tail_number = 0;
    // Other: an expression that binds looser than + and − (a .. b, a < b, a and b, if … then … else …, x :: T):
    // a correction goes after it in parentheses.
    bool loose = false;
};

struct CodeField {
    std::string name;
    u32 key = 0; // where the item starts
    CodeArg value;
};
// A table's field of that name, as the table has it: the last written, since Lua assigns the fields in order
// ({ y = 1, y = 2 } has y 2). Null when it has none or is not a table.
const CodeField* field_of(const CodeArg& table, std::string_view name);

struct CallSite {
    u32 line = 0;          // 1-based: the line of a method's name, else of the arguments' "("
    CodeSpan span;         // the whole call
    std::string name;      // "y" in body.y(…), "to" in t:to(…), "wait" in wait(…); empty when no name is called
    bool method = false;   // o:m(…)
    bool parens = true;    // false for f"…" and f{…}
    u32 open = 0, close = 0; // "(" and ")" when parens
    u32 callee_end = 0;    // past f, or past o in o:m(…)
    std::vector<CodeArg> args; // a method's self not among them
};

// The clip functions of the module: return { clips = { walk = function(p) … end } }.
struct ClipFunction {
    std::string name;
    u32 params_end = 0;   // past ")" of its parameters (and a return type, if any)
    u32 header_line = 0;  // 1-based, of that ")"
    i32 first = -1;       // where its first statement starts; -1 when its body is empty
    u32 first_line = 0;   // and its line
};

// A drawing function of the module (part 4е of docs/models/движение-модель.md): draw = function(p, rng) … end, or an
// item of drawings = { open = function(p, rng) … end }; where a new last line of it goes.
struct DrawingFunction {
    std::string name;         // "draw", else the drawing's name
    u32 line = 0;             // 1-based, of its key
    bool function = false;    // written here as function … end; false for draw = shared.draw and the like
    u32 end = 0;              // where its "end" starts
    i32 last = -1;            // where its last statement starts; -1 when its body is empty
    bool last_return = false; // that statement is return …
};

class CodeSites {
public:
    // False with why (the parser's words) and its line when the code does not parse; sites() is then empty.
    bool parse(std::string_view source, std::string& why, u32& why_line);
    const std::vector<CallSite>& sites() const { return sites_; }
    const std::vector<ClipFunction>& clips() const { return clips_; }
    const ClipFunction* clip(std::string_view name) const;
    // The line of a key of the module's table (return { draw = …, drawings = … }) and of an item of its drawings
    // (drawings = { angry = function(p, rng) … end }, part 4д); 0 when there is none.
    u32 module_line(std::string_view key) const;
    u32 drawing_line(std::string_view name) const;
    // The drawing functions: draw, then the items of drawings in their order; by name, "draw" for draw (null for none).
    const std::vector<DrawingFunction>& drawing_functions() const { return functions_; }
    const DrawingFunction* drawing_function(std::string_view name) const;

    // The source with every call through __fc / __fm, numbered from first_id in sites() order: f(a, b) is
    // __fc(id, f, a, b) and o:m(a) is __fm(id, o, "m", a). Every token stays on its line.
    std::string instrumented(u32 first_id) const;

private:
    std::string source_;
    std::vector<CallSite> sites_;
    std::vector<ClipFunction> clips_;
    std::vector<std::pair<std::string, u32>> keys_, drawings_;
    std::vector<DrawingFunction> functions_;
};

// A number as the editor writes it: to thousandths, without trailing zeros ("0.25", "-3", "12.5").
std::string format_number(f64 v);
// A Luau string literal of the text.
std::string quote_string(std::string_view text);

// The rewrites of a mouse edit. `source` is the text the sites were parsed from; out is the whole new text, the same
// but for the characters named. False with why (Russian) when it cannot be done; out is then empty.

// The value of an argument: a number literal is written anew; an expression keeps itself and gets a correction by
// value − old_value (-p.height → -p.height - 2), or its "± n" at the end changes. No change when they round equal.
bool rewrite_number(std::string_view source, const CodeArg& arg, f64 old_value, f64 value, std::string& out,
                    std::string& why);
// Argument `index` of a call becomes `text`; one past the last is added (", text"). f{…} and f"…" get parentheses.
bool rewrite_arg(std::string_view source, const CallSite& site, usize index, std::string_view text, std::string& out,
                 std::string& why);
// A table's field `name` becomes `text`, added when the table has none: {y = 2} → {y = 2, x = 3}.
bool rewrite_field(std::string_view source, const CodeArg& table, std::string_view name, std::string_view text,
                   std::string& out, std::string& why);
// Its number: as rewrite_number for a field there, added when missing.
bool rewrite_field_number(std::string_view source, const CodeArg& table, std::string_view name, f64 old_value,
                          f64 value, std::string& out, std::string& why);
// A new first line of a clip function: at(0.25, part("head").rot(5, 0.1)).
bool insert_clip_line(const CodeSites& code, std::string_view source, std::string_view clip, std::string_view line,
                      std::string& out, std::string& why);
// A new last line of drawing function `name` ("draw", or an item of drawings), part 4е: before the line of its "end",
// indented as its last statement (an empty one as its "end" and two spaces more); before a return that ends it; on the
// line of an "end" that shares it, before it. A file of CRLF lines stays one. False with why.
bool insert_drawing_line(const CodeSites& code, std::string_view source, std::string_view name, std::string_view line,
                         std::string& out, std::string& why);

} // namespace forge::pixel
