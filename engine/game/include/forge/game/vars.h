#pragma once

// Game variables and the small expression language that reads and changes
// them. Dialogues, quests and save files use these: "quest.pickaxe >= 2 and
// not met_miner" picks a dialogue branch, "gold += 10" is a dialogue action.
//
// A variable is a number or a text. A missing variable reads as 0, so a new
// flag needs no declaration. Names may contain dots ("quest.pickaxe").
//
// Expressions:
//   numbers 3, 2.5   texts "кирка"   true / false   variables a.b.c
//   + - * /   == != < <= > >=   and or not   ( )
//   function calls name(args...), answered by the game (has("pickaxe")).
// Statements (actions): name = expr, name += expr, name -= expr, or a call.

#include "forge/core/types.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace forge::game {

class Value {
public:
    Value() = default;
    Value(f64 number) : v_(number) {}
    Value(i32 number) : v_(static_cast<f64>(number)) {}
    Value(bool flag) : v_(flag ? 1.0 : 0.0) {}
    Value(std::string text) : v_(std::move(text)) {}
    Value(const char* text) : v_(std::string(text)) {}

    bool is_text() const { return v_.index() == 1; }
    f64 number() const; // a text reads as its number, or 0
    std::string text() const; // numbers without trailing zeros: 3, 2.5
    bool truthy() const { return is_text() ? !std::get<1>(v_).empty() : std::get<0>(v_) != 0; }
    bool operator==(const Value& o) const;

private:
    std::variant<f64, std::string> v_ = 0.0;
};

class Vars {
public:
    Value get(std::string_view name) const;
    void set(std::string_view name, Value value);
    bool has(std::string_view name) const { return values_.find(std::string(name)) != values_.end(); }
    void clear() { values_.clear(); }
    const std::map<std::string, Value, std::less<>>& all() const { return values_; }
    // Bumped by every set(); views compare it to know when to refresh.
    u64 version() const { return version_; }

    // {"gold": 10, "hero.name": "Борин"}
    std::string to_json() const;
    bool from_json(std::string_view json, std::string* error = nullptr);

private:
    std::map<std::string, Value, std::less<>> values_;
    u64 version_ = 0;
};

// Functions an expression may call, answered by the game. Unknown names
// read as 0 (and are reported once by the validator, not at run time).
using CallFn = std::function<Value(std::string_view name, const std::vector<Value>& args)>;

struct ExprNode;

// A parsed expression or statement list. Parsing happens once (when a
// dialogue loads); evaluating is cheap.
class Expr {
public:
    Expr();
    ~Expr();
    Expr(Expr&&) noexcept;
    Expr& operator=(Expr&&) noexcept;

    // An empty source gives an expression that is true.
    static Expr parse(std::string_view source, std::string* error);
    // Statements separated by ';' or given one per string.
    static Expr parse_actions(std::string_view source, std::string* error);

    bool empty() const { return root_ == nullptr; }
    Value eval(const Vars& vars, const CallFn& call = {}) const;
    bool test(const Vars& vars, const CallFn& call = {}) const { return empty() || eval(vars, call).truthy(); }
    // Runs statements: assignments change vars, calls go to call.
    void run(Vars& vars, const CallFn& call = {}) const;

    // Names of the functions it calls and of the variables it reads or
    // writes (for validators and editors).
    void collect(std::vector<std::string>* calls, std::vector<std::string>* vars) const;

private:
    std::unique_ptr<ExprNode> root_;
};

// "Привет, {hero.name}!" with variables put in; one nothing set stays as
// written ("{hero.name}"). "{{" gives "{".
std::string substitute(std::string_view text, const Vars& vars);

} // namespace forge::game
