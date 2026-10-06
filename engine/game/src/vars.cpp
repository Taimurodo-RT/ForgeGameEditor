#include "forge/game/vars.h"

#include <yyjson.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace forge::game {

// --- Value -----------------------------------------------------------------

f64 Value::number() const {
    if (!is_text()) return std::get<0>(v_);
    const std::string& s = std::get<1>(v_);
    char* end = nullptr;
    const f64 n = std::strtod(s.c_str(), &end);
    return end && end != s.c_str() && *end == 0 ? n : 0.0;
}

std::string Value::text() const {
    if (is_text()) return std::get<1>(v_);
    const f64 n = std::get<0>(v_);
    char buf[32];
    if (n == std::floor(n) && std::fabs(n) < 1e15) std::snprintf(buf, sizeof(buf), "%.0f", n);
    else std::snprintf(buf, sizeof(buf), "%g", n);
    return buf;
}

bool Value::operator==(const Value& o) const {
    if (is_text() && o.is_text()) return std::get<1>(v_) == std::get<1>(o.v_);
    if (!is_text() && !o.is_text()) return std::get<0>(v_) == std::get<0>(o.v_);
    return text() == o.text(); // 3 == "3"
}

// --- Vars ------------------------------------------------------------------

Value Vars::get(std::string_view name) const {
    auto it = values_.find(name);
    return it == values_.end() ? Value() : it->second;
}

void Vars::set(std::string_view name, Value value) {
    auto it = values_.find(name);
    if (it == values_.end()) values_.emplace(std::string(name), std::move(value));
    else it->second = std::move(value);
    ++version_;
}

std::string Vars::to_json() const {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    for (const auto& [name, value] : values_) {
        yyjson_mut_val* key = yyjson_mut_strncpy(doc, name.data(), name.size());
        yyjson_mut_val* val;
        if (value.is_text()) {
            const std::string t = value.text();
            val = yyjson_mut_strncpy(doc, t.data(), t.size());
        } else {
            val = yyjson_mut_real(doc, value.number());
        }
        yyjson_mut_obj_add(root, key, val);
    }
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
    std::string out = text ? std::string(text, len) : "{}";
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

bool Vars::from_json(std::string_view json, std::string* error) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!root || !yyjson_is_obj(root)) {
        if (error) *error = "variables: not a JSON object";
        if (doc) yyjson_doc_free(doc);
        return false;
    }
    values_.clear();
    yyjson_obj_iter it = yyjson_obj_iter_with(root);
    for (yyjson_val* key; (key = yyjson_obj_iter_next(&it));) {
        yyjson_val* val = yyjson_obj_iter_get_val(key);
        const std::string name(yyjson_get_str(key), yyjson_get_len(key));
        if (yyjson_is_str(val)) values_[name] = Value(std::string(yyjson_get_str(val), yyjson_get_len(val)));
        else if (yyjson_is_num(val)) values_[name] = Value(yyjson_get_num(val));
        else if (yyjson_is_bool(val)) values_[name] = Value(yyjson_get_bool(val));
    }
    yyjson_doc_free(doc);
    ++version_;
    return true;
}

// --- expressions -----------------------------------------------------------

enum class Op : u8 {
    Const, Var, Call,
    Neg, Not, And, Or,
    Add, Sub, Mul, Div,
    Eq, Ne, Lt, Le, Gt, Ge,
    Assign, AddAssign, SubAssign, Seq,
};

struct ExprNode {
    Op op = Op::Const;
    Value value;            // Const
    std::string name;       // Var, Call, assignments
    std::vector<std::unique_ptr<ExprNode>> args;
};

namespace {

enum class Tok : u8 { End, Number, Text, Name, Sym, Bad };

struct Token {
    Tok kind = Tok::End;
    std::string text;
    f64 number = 0;
};

bool name_start(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool name_char(unsigned char c) { return name_start(c) || std::isdigit(c) || c == '.'; }

class Parser {
public:
    Parser(std::string_view src, std::string* error) : s_(src), error_(error) { next(); }

    std::unique_ptr<ExprNode> expression() {
        auto e = parse_or();
        return e;
    }

    std::unique_ptr<ExprNode> statements() {
        auto seq = std::make_unique<ExprNode>();
        seq->op = Op::Seq;
        while (tok_.kind != Tok::End && !failed_) {
            if (is_sym(";")) {
                next();
                continue;
            }
            seq->args.push_back(statement());
        }
        return seq;
    }

    bool at_end() const { return tok_.kind == Tok::End; }
    bool failed() const { return failed_; }
    void fail(const std::string& what) {
        if (failed_) return;
        failed_ = true;
        if (error_) *error_ = what;
    }
    const Token& token() const { return tok_; }

private:
    std::unique_ptr<ExprNode> statement() {
        if (tok_.kind != Tok::Name) {
            fail("ожидалось имя переменной или функции");
            return nullptr;
        }
        std::string name = tok_.text;
        next();
        if (is_sym("(")) return call(std::move(name));
        Op op;
        if (is_sym("=")) op = Op::Assign;
        else if (is_sym("+=")) op = Op::AddAssign;
        else if (is_sym("-=")) op = Op::SubAssign;
        else {
            fail("после «" + name + "» ожидалось =, += или -=");
            return nullptr;
        }
        next();
        auto n = std::make_unique<ExprNode>();
        n->op = op;
        n->name = std::move(name);
        n->args.push_back(parse_or());
        return n;
    }

    std::unique_ptr<ExprNode> call(std::string name) {
        auto n = std::make_unique<ExprNode>();
        n->op = Op::Call;
        n->name = std::move(name);
        next(); // (
        if (!is_sym(")")) {
            for (;;) {
                n->args.push_back(parse_or());
                if (failed_) return n;
                if (is_sym(",")) {
                    next();
                    continue;
                }
                break;
            }
        }
        if (!is_sym(")")) fail("в вызове «" + n->name + "» не хватает )");
        else next();
        return n;
    }

    std::unique_ptr<ExprNode> binary(Op op, std::unique_ptr<ExprNode> a, std::unique_ptr<ExprNode> b) {
        auto n = std::make_unique<ExprNode>();
        n->op = op;
        n->args.push_back(std::move(a));
        n->args.push_back(std::move(b));
        return n;
    }

    bool word(const char* w) const { return tok_.kind == Tok::Name && tok_.text == w; }
    bool is_sym(const char* s) const { return tok_.kind == Tok::Sym && tok_.text == s; }

    std::unique_ptr<ExprNode> parse_or() {
        auto a = parse_and();
        while (!failed_ && (word("or") || word("или"))) {
            next();
            a = binary(Op::Or, std::move(a), parse_and());
        }
        return a;
    }
    std::unique_ptr<ExprNode> parse_and() {
        auto a = parse_not();
        while (!failed_ && (word("and") || word("и"))) {
            next();
            a = binary(Op::And, std::move(a), parse_not());
        }
        return a;
    }
    std::unique_ptr<ExprNode> parse_not() {
        if (word("not") || word("не")) {
            next();
            auto n = std::make_unique<ExprNode>();
            n->op = Op::Not;
            n->args.push_back(parse_not());
            return n;
        }
        return parse_cmp();
    }
    std::unique_ptr<ExprNode> parse_cmp() {
        auto a = parse_sum();
        static const std::pair<const char*, Op> ops[] = {{"==", Op::Eq}, {"!=", Op::Ne}, {"<=", Op::Le},
                                                         {">=", Op::Ge}, {"<", Op::Lt},  {">", Op::Gt}};
        for (const auto& [s, op] : ops)
            if (!failed_ && is_sym(s)) {
                next();
                return binary(op, std::move(a), parse_sum());
            }
        return a;
    }
    std::unique_ptr<ExprNode> parse_sum() {
        auto a = parse_term();
        while (!failed_ && (is_sym("+") || is_sym("-"))) {
            const Op op = is_sym("+") ? Op::Add : Op::Sub;
            next();
            a = binary(op, std::move(a), parse_term());
        }
        return a;
    }
    std::unique_ptr<ExprNode> parse_term() {
        auto a = parse_unary();
        while (!failed_ && (is_sym("*") || is_sym("/"))) {
            const Op op = is_sym("*") ? Op::Mul : Op::Div;
            next();
            a = binary(op, std::move(a), parse_unary());
        }
        return a;
    }
    std::unique_ptr<ExprNode> parse_unary() {
        if (is_sym("-")) {
            next();
            auto n = std::make_unique<ExprNode>();
            n->op = Op::Neg;
            n->args.push_back(parse_unary());
            return n;
        }
        return parse_atom();
    }
    std::unique_ptr<ExprNode> parse_atom() {
        auto n = std::make_unique<ExprNode>();
        switch (tok_.kind) {
        case Tok::Number:
            n->value = Value(tok_.number);
            next();
            return n;
        case Tok::Text:
            n->value = Value(tok_.text);
            next();
            return n;
        case Tok::Name: {
            std::string name = tok_.text;
            next();
            if (name == "true" || name == "да") n->value = Value(true);
            else if (name == "false" || name == "нет") n->value = Value(false);
            else if (is_sym("(")) return call(std::move(name));
            else {
                n->op = Op::Var;
                n->name = std::move(name);
            }
            return n;
        }
        case Tok::Sym:
            if (is_sym("(")) {
                next();
                auto e = parse_or();
                if (!is_sym(")")) fail("не хватает )");
                else next();
                return e;
            }
            fail("неожиданный знак «" + tok_.text + "»");
            return n;
        case Tok::End: fail("выражение оборвалось"); return n;
        default: fail("непонятный символ «" + tok_.text + "»"); return n;
        }
    }

    void next() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        tok_ = {};
        if (pos_ >= s_.size()) return;
        const unsigned char c = static_cast<unsigned char>(s_[pos_]);
        if (std::isdigit(c) || (c == '.' && pos_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])))) {
            const char* begin = s_.data() + pos_;
            char* end = nullptr;
            const std::string tmp(begin, std::min<usize>(s_.size() - pos_, 64));
            tok_.number = std::strtod(tmp.c_str(), &end);
            pos_ += static_cast<usize>(end - tmp.c_str());
            tok_.kind = Tok::Number;
            return;
        }
        if (c == '"' || c == '\'') {
            const char quote = static_cast<char>(c);
            ++pos_;
            tok_.kind = Tok::Text;
            while (pos_ < s_.size() && s_[pos_] != quote) {
                if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) ++pos_;
                tok_.text += s_[pos_++];
            }
            if (pos_ >= s_.size()) fail("текст в кавычках не закрыт");
            else ++pos_;
            return;
        }
        if (name_start(c)) {
            const usize begin = pos_;
            while (pos_ < s_.size() && name_char(static_cast<unsigned char>(s_[pos_]))) ++pos_;
            tok_.kind = Tok::Name;
            tok_.text.assign(s_.substr(begin, pos_ - begin));
            return;
        }
        static const char* two[] = {"==", "!=", "<=", ">=", "+=", "-=", "&&", "||"};
        for (const char* t : two)
            if (s_.substr(pos_, 2) == t) {
                pos_ += 2;
                tok_.kind = Tok::Name; // && and || read as the words
                if (std::strcmp(t, "&&") == 0) tok_.text = "and";
                else if (std::strcmp(t, "||") == 0) tok_.text = "or";
                else {
                    tok_.kind = Tok::Sym;
                    tok_.text = t;
                }
                return;
            }
        if (std::strchr("+-*/<>=()!,;", c)) {
            tok_.kind = Tok::Sym;
            tok_.text.assign(1, static_cast<char>(c));
            ++pos_;
            if (tok_.text == "!") {
                tok_.kind = Tok::Name;
                tok_.text = "not";
            }
            return;
        }
        tok_.kind = Tok::Bad;
        tok_.text.assign(1, static_cast<char>(c));
        ++pos_;
    }

    std::string_view s_;
    usize pos_ = 0;
    Token tok_;
    std::string* error_;
    bool failed_ = false;
};

Value eval_node(const ExprNode& n, const Vars& vars, const CallFn& call) {
    auto arg = [&](usize i) { return eval_node(*n.args[i], vars, call); };
    switch (n.op) {
    case Op::Const: return n.value;
    case Op::Var: return vars.get(n.name);
    case Op::Call: {
        std::vector<Value> args;
        args.reserve(n.args.size());
        for (usize i = 0; i < n.args.size(); ++i) args.push_back(arg(i));
        return call ? call(n.name, args) : Value();
    }
    case Op::Neg: return Value(-arg(0).number());
    case Op::Not: return Value(!arg(0).truthy());
    case Op::And: return Value(arg(0).truthy() && arg(1).truthy());
    case Op::Or: return Value(arg(0).truthy() || arg(1).truthy());
    case Op::Add: {
        const Value a = arg(0), b = arg(1);
        if (a.is_text() || b.is_text()) return Value(a.text() + b.text());
        return Value(a.number() + b.number());
    }
    case Op::Sub: return Value(arg(0).number() - arg(1).number());
    case Op::Mul: return Value(arg(0).number() * arg(1).number());
    case Op::Div: {
        const f64 d = arg(1).number();
        return Value(d == 0 ? 0.0 : arg(0).number() / d);
    }
    case Op::Eq: return Value(arg(0) == arg(1));
    case Op::Ne: return Value(!(arg(0) == arg(1)));
    case Op::Lt: return Value(arg(0).number() < arg(1).number());
    case Op::Le: return Value(arg(0).number() <= arg(1).number());
    case Op::Gt: return Value(arg(0).number() > arg(1).number());
    case Op::Ge: return Value(arg(0).number() >= arg(1).number());
    default: return Value();
    }
}

void run_node(const ExprNode& n, Vars& vars, const CallFn& call) {
    switch (n.op) {
    case Op::Seq:
        for (const auto& s : n.args) run_node(*s, vars, call);
        return;
    case Op::Assign: vars.set(n.name, eval_node(*n.args[0], vars, call)); return;
    case Op::AddAssign: {
        const Value cur = vars.get(n.name), add = eval_node(*n.args[0], vars, call);
        if (cur.is_text() || add.is_text()) vars.set(n.name, Value(cur.text() + add.text()));
        else vars.set(n.name, Value(cur.number() + add.number()));
        return;
    }
    case Op::SubAssign: vars.set(n.name, Value(vars.get(n.name).number() - eval_node(*n.args[0], vars, call).number())); return;
    default: (void)eval_node(n, vars, call); return; // a call
    }
}

void collect_node(const ExprNode& n, std::vector<std::string>* calls, std::vector<std::string>* names) {
    if (n.op == Op::Call && calls) calls->push_back(n.name);
    if ((n.op == Op::Var || n.op == Op::Assign || n.op == Op::AddAssign || n.op == Op::SubAssign) && names)
        names->push_back(n.name);
    for (const auto& a : n.args)
        if (a) collect_node(*a, calls, names);
}

} // namespace

Expr::Expr() = default;
Expr::~Expr() = default;
Expr::Expr(Expr&&) noexcept = default;
Expr& Expr::operator=(Expr&&) noexcept = default;

Expr Expr::parse(std::string_view source, std::string* error) {
    Expr e;
    Parser p(source, error);
    if (p.at_end()) return e;
    auto root = p.expression();
    if (!p.failed() && !p.at_end()) p.fail("лишнее в конце: «" + p.token().text + "»");
    if (!p.failed()) e.root_ = std::move(root);
    return e;
}

Expr Expr::parse_actions(std::string_view source, std::string* error) {
    Expr e;
    Parser p(source, error);
    if (p.at_end()) return e;
    auto root = p.statements();
    if (!p.failed()) e.root_ = std::move(root);
    return e;
}

Value Expr::eval(const Vars& vars, const CallFn& call) const { return root_ ? eval_node(*root_, vars, call) : Value(true); }

void Expr::run(Vars& vars, const CallFn& call) const {
    if (root_) run_node(*root_, vars, call);
}

void Expr::collect(std::vector<std::string>* calls, std::vector<std::string>* names) const {
    if (root_) collect_node(*root_, calls, names);
}

std::string substitute(std::string_view text, const Vars& vars) {
    std::string out;
    out.reserve(text.size());
    for (usize i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            if (i + 1 < text.size() && text[i + 1] == '{') {
                out += '{';
                ++i;
                continue;
            }
            const usize close = text.find('}', i);
            if (close != std::string_view::npos) {
                // A variable nothing set yet shows as written: «Привет, {player}!».
                const std::string_view name = text.substr(i + 1, close - i - 1);
                if (vars.has(name)) out += vars.get(name).text();
                else out += text.substr(i, close - i + 1);
                i = close;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

} // namespace forge::game
