#include "forge/script/compiler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace forge::script {

namespace {

std::string trim(std::string_view s) {
    usize a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

bool parse_number(std::string_view text, f64& out) {
    const std::string t = trim(text);
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == 0 && std::isfinite(out);
}

std::string number_lua(f64 v) {
    if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case 0: out += "\\0"; break;
        default: out += c;
        }
    }
    return out + "\"";
}

// "1, 2" or "1 2" -> numbers.
bool parse_numbers(std::string_view text, std::vector<f64>& out) {
    out.clear();
    std::string cur;
    auto flush = [&]() {
        if (cur.empty()) return true;
        f64 v;
        if (!parse_number(cur, v)) return false;
        out.push_back(v);
        cur.clear();
        return true;
    };
    for (char c : text) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) {
            if (!flush()) return false;
        } else {
            cur += c;
        }
    }
    return flush();
}

bool is_ident(std::string_view s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

// Pin ids become parts of Luau names; anything odd is replaced.
std::string ident_part(std::string_view s) {
    std::string out;
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    return out;
}

const char* type_title(ValueType t) {
    switch (t) {
    case ValueType::Any: return "любое";
    case ValueType::Bool: return "да/нет";
    case ValueType::Number: return "число";
    case ValueType::Integer: return "целое";
    case ValueType::String: return "текст";
    case ValueType::Vec2: return "вектор";
    case ValueType::Color: return "цвет";
    case ValueType::Entity: return "объект";
    case ValueType::Asset: return "ассет";
    case ValueType::Enum: return "вариант";
    case ValueType::Table: return "список";
    }
    return "?";
}

} // namespace

bool value_to_lua(ValueType type, std::string_view text, std::string& out) {
    const std::string t = trim(text);
    f64 n = 0;
    std::vector<f64> nums;
    switch (type) {
    case ValueType::Bool:
        if (t == "true" || t == "1" || t == "да") out = "true";
        else if (t == "false" || t == "0" || t == "нет" || t.empty()) out = "false";
        else return false;
        return true;
    case ValueType::Number:
    case ValueType::Integer:
        if (t.empty()) {
            out = "0";
            return true;
        }
        if (!parse_number(t, n)) return false;
        out = number_lua(type == ValueType::Integer ? std::floor(n) : n);
        return true;
    case ValueType::String:
    case ValueType::Asset:
    case ValueType::Enum:
        out = quote(text);
        return true;
    case ValueType::Vec2:
        if (!parse_numbers(t, nums) || nums.size() > 2) return false;
        nums.resize(2, 0);
        out = "vector.create(" + number_lua(nums[0]) + ", " + number_lua(nums[1]) + ", 0)";
        return true;
    case ValueType::Color:
        if (!parse_numbers(t, nums) || nums.size() > 4) return false;
        while (nums.size() < 4) nums.push_back(1);
        out = "{r = " + number_lua(nums[0]) + ", g = " + number_lua(nums[1]) + ", b = " + number_lua(nums[2]) +
              ", a = " + number_lua(nums[3]) + "}";
        return true;
    case ValueType::Entity:
        if (t == "self" || t == "этот") out = "self";
        else if (t.empty()) out = "0";
        else if (parse_number(t, n)) out = number_lua(n);
        else return false;
        return true;
    case ValueType::Table:
        if (!t.empty()) return false;
        out = "{}";
        return true;
    case ValueType::Any:
        if (t.empty()) out = "nil";
        else if (t == "true" || t == "false") out = t;
        else if (t == "self") out = "self";
        else if (parse_number(t, n)) out = number_lua(n);
        else out = quote(text);
        return true;
    }
    return false;
}

namespace {

struct Emitter {
    std::string text;
    std::vector<u32> line_node;
    u32 node = SourceMap::kNoNode;

    void line(int indent, std::string_view s) {
        text.append(static_cast<usize>(indent) * 4, ' ');
        text += s;
        text += '\n';
        line_node.push_back(node);
    }
};

class GraphCompiler {
public:
    GraphCompiler(const Graph& g, const NodeLibrary& lib, const CompileOptions& opt, Emitter& em,
                  std::vector<Diagnostic>& diags, std::set<std::string>& macros_done, std::vector<std::string>& macro_stack,
                  bool inner)
        : g_(g), lib_(lib), opt_(opt), em_(em), diags_(diags), macros_done_(macros_done), macro_stack_(macro_stack),
          inner_(inner) {}

    bool index();
    void emit_macros();                 // graph nodes this graph uses, as local functions (before anything else)
    void emit_handlers();               // a script: every event
    void emit_macro_body(const std::string& fname); // a graph used as a node

private:
    struct Out {
        u32 node;
        std::string pin;
    };

    void error(u32 node, std::string_view pin, std::string msg) {
        if (inner_) msg = "в ноде-графе «" + g_.name + "»: " + msg, node = 0;
        diags_.push_back({true, node, std::string(pin), std::move(msg)});
    }
    void warning(u32 node, std::string msg) {
        if (inner_) return;
        diags_.push_back({false, node, {}, std::move(msg)});
    }

    const NodeDef* def(u32 uid) const {
        auto it = defs_.find(uid);
        return it == defs_.end() ? nullptr : it->second;
    }
    std::string title(u32 uid) const {
        const NodeDef* d = def(uid);
        return d ? d->title.get() : std::string("?");
    }
    // Pins of a node; "Вход"/"Результат" of a graph-node take theirs from the graph.
    const PinDef* out_pin(u32 uid, std::string_view pin) const {
        const NodeDef* d = def(uid);
        if (!d) return nullptr;
        if (d->id == "std.graph.entry") {
            for (const PinDef& p : g_.inputs)
                if (p.id == pin) return &p;
            return nullptr;
        }
        return d->output(pin);
    }
    const PinDef* in_pin(u32 uid, std::string_view pin) const {
        const NodeDef* d = def(uid);
        if (!d) return nullptr;
        if (d->id == "std.graph.return") {
            for (const PinDef& p : g_.outputs)
                if (p.id == pin) return &p;
            return nullptr;
        }
        return d->input(pin);
    }
    std::string pin_title(const PinDef& p) const { return p.title.get().empty() ? p.id : p.title.get(); }

    u32 flow_target(u32 uid, const std::string& exit) const {
        auto it = flow_out_.find({uid, exit});
        return it == flow_out_.end() ? 0 : it->second;
    }

    std::string out_var(u32 uid, std::string_view pin) const;
    std::string input_expr(u32 uid, const PinDef& pin);
    void collect_pure(u32 uid, std::vector<u32>& order, std::unordered_set<u32>& seen, std::unordered_set<u32>& visiting);
    void emit_pure(u32 uid, int ind);
    void emit_statement(u32 uid, int ind);
    void emit_node_code(u32 uid, int ind);
    void emit_template(u32 uid, const std::string& tmpl, int ind, u32 chain_after);
    std::string expand(u32 uid, const std::string& line);
    void emit_slot(u32 uid, const std::string& slot, int ind);
    void emit_chain(u32 uid, int ind, u32 start_fn = 0);
    void reach(u32 uid, std::vector<u32>& order, std::unordered_set<u32>& seen);
    void emit_function_body(u32 first, int ind);
    std::string macro_name(const NodeDef& d) const { return "g_" + ident_part(d.graph); }

    const Graph& g_;
    const NodeLibrary& lib_;
    const CompileOptions& opt_;
    Emitter& em_;
    std::vector<Diagnostic>& diags_;
    std::set<std::string>& macros_done_;
    std::vector<std::string>& macro_stack_;
    bool inner_;

    std::unordered_map<u32, const GraphNode*> nodes_;
    std::unordered_map<u32, const NodeDef*> defs_;
    std::map<std::pair<u32, std::string>, Out> data_in_;    // (to, pin) -> from
    std::map<std::pair<u32, std::string>, u32> flow_out_;   // (from, exit) -> to
    std::unordered_map<u32, u32> incoming_;                 // flow wires into a node, plus its slot place
    std::unordered_map<u32, u32> parent_;                   // block -> the node whose slot holds it
    std::unordered_set<u32> chain_fns_;                     // nodes with several ways in: local functions
};

bool GraphCompiler::index() {
    bool ok = true;
    for (const GraphNode& n : g_.nodes) {
        nodes_[n.uid] = &n;
        const NodeDef* d = lib_.find(n.def);
        if (!d) {
            error(n.uid, {}, "нода «" + n.def + "» не найдена в библиотеке (модуль не подключён?)");
            ok = false;
            continue;
        }
        if (n.version > d->version)
            warning(n.uid, "нода «" + d->title.get() + "» сделана более новой версией библиотеки");
        defs_[n.uid] = d;
    }
    for (const GraphNode& n : g_.nodes)
        for (const SlotContent& s : n.slots) {
            const NodeDef* d = def(n.uid);
            if (d && !d->slot(s.slot)) error(n.uid, s.slot, "у ноды «" + d->title.get() + "» нет слота «" + s.slot + "»");
            for (u32 c : s.nodes) {
                if (!nodes_.count(c)) {
                    error(n.uid, s.slot, "в слоте лежит удалённая нода");
                    ok = false;
                    continue;
                }
                if (parent_.count(c)) error(c, {}, "блок лежит сразу в двух слотах");
                parent_[c] = n.uid;
                ++incoming_[c];
            }
        }
    for (const GraphLink& l : g_.links) {
        const NodeDef* from = def(l.from_node);
        const NodeDef* to = def(l.to_node);
        if (!from || !to) {
            if (!nodes_.count(l.from_node) || !nodes_.count(l.to_node)) error(0, {}, "провод ведёт к удалённой ноде");
            continue;
        }
        const bool flow = l.to_pin == kFlowIn;
        if (flow) {
            const bool exit_ok = (l.from_pin == kFlowNext && from->has_next()) || from->slot(l.from_pin);
            if (!exit_ok) {
                error(l.from_node, l.from_pin, "у ноды «" + from->title.get() + "» нет выхода «" + l.from_pin + "»");
                ok = false;
                continue;
            }
            if (!to->has_flow_in()) {
                error(l.to_node, l.to_pin, "в ноду «" + to->title.get() + "» нельзя провести поток");
                ok = false;
                continue;
            }
            auto [it, added] = flow_out_.insert({{l.from_node, l.from_pin}, l.to_node});
            if (!added) {
                error(l.from_node, l.from_pin, "из одного выхода может идти только один провод потока");
                ok = false;
                continue;
            }
            ++incoming_[l.to_node];
        } else {
            const PinDef* out = out_pin(l.from_node, l.from_pin);
            const PinDef* in = in_pin(l.to_node, l.to_pin);
            if (!out) {
                error(l.from_node, l.from_pin, "у ноды «" + from->title.get() + "» нет выхода «" + l.from_pin + "»");
                ok = false;
                continue;
            }
            if (!in) {
                error(l.to_node, l.to_pin, "у ноды «" + to->title.get() + "» нет входа «" + l.to_pin + "»");
                ok = false;
                continue;
            }
            if (in->constant) {
                error(l.to_node, l.to_pin, "«" + pin_title(*in) + "» задаётся значением, не проводом");
                ok = false;
                continue;
            }
            auto [it, added] = data_in_.insert({{l.to_node, l.to_pin}, {l.from_node, l.from_pin}});
            if (!added) {
                error(l.to_node, l.to_pin, "ко входу «" + pin_title(*in) + "» подключено два провода");
                ok = false;
            }
        }
    }
    for (const auto& [uid, n] : incoming_)
        if (n > 1) chain_fns_.insert(uid);
    // Wires that go round in circles would call each other forever.
    std::unordered_map<u32, int> state; // 1 visiting, 2 done
    u32 loop_node = 0;
    std::function<bool(u32)> cyclic = [&](u32 u) -> bool {
        state[u] = 1;
        for (const auto& [key, to] : flow_out_)
            if (key.first == u) {
                if (state[to] == 1) {
                    loop_node = to;
                    return true;
                }
                if (state[to] == 0 && cyclic(to)) return true;
            }
        state[u] = 2;
        return false;
    };
    for (const auto& [key, to] : flow_out_)
        if (state[key.first] == 0 && cyclic(key.first)) {
            error(loop_node, {}, "провода потока замкнуты в круг: для повторов используйте «Повторить» или «Пока»");
            return false;
        }
    return ok;
}

std::string GraphCompiler::out_var(u32 uid, std::string_view pin) const {
    const NodeDef* d = def(uid);
    if (d && d->id == "std.graph.entry") return "i_" + ident_part(pin);
    if (d && d->kind == NodeKind::Event) return "p_" + ident_part(pin);
    if (d && d->kind == NodeKind::Pure) return "v" + std::to_string(uid) + "_" + ident_part(pin);
    return "o" + std::to_string(uid) + "_" + ident_part(pin);
}

std::string GraphCompiler::input_expr(u32 uid, const PinDef& pin) {
    auto link = data_in_.find({uid, pin.id});
    if (link != data_in_.end()) {
        const Out& src = link->second;
        const PinDef* sp = out_pin(src.node, src.pin);
        std::string expr = out_var(src.node, src.pin);
        const ValueType from = sp ? sp->type : ValueType::Any;
        const ValueType to = pin.type;
        if (from == to || from == ValueType::Any || to == ValueType::Any) return expr;
        if ((from == ValueType::Integer && to == ValueType::Number) || (from == ValueType::Number && to == ValueType::Integer))
            return to == ValueType::Integer ? "math.floor(" + expr + ")" : expr;
        if (to == ValueType::Bool && from == ValueType::Entity) return "forge.entity.alive(" + expr + ")";
        if (to == ValueType::String &&
            (from == ValueType::Number || from == ValueType::Integer || from == ValueType::Bool || from == ValueType::Enum))
            return "tostring(" + expr + ")";
        if (from == ValueType::Enum && to == ValueType::String) return expr;
        if (from == ValueType::String && (to == ValueType::Enum || to == ValueType::Asset)) return expr;
        error(uid, pin.id,
              std::string("провод несёт «") + type_title(from) + "», а входу «" + pin_title(pin) + "» нужно «" + type_title(to) + "»");
        return "nil";
    }
    const GraphNode* n = nodes_.at(uid);
    const std::string* v = n->value(pin.id);
    const std::string text = v ? *v : pin.value;
    if (trim(text).empty() && pin.required) {
        error(uid, pin.id, "Подключите «" + pin_title(pin) + "»: без него нода «" + title(uid) + "» не сработает");
        return "nil";
    }
    if (trim(text).empty() && pin.type != ValueType::String && pin.type != ValueType::Bool &&
        pin.type != ValueType::Number && pin.type != ValueType::Integer)
        return "nil";
    std::string lua;
    if (!value_to_lua(pin.type, text, lua)) {
        error(uid, pin.id, "«" + text + "» не подходит для «" + pin_title(pin) + "» (" + type_title(pin.type) + ")");
        return "nil";
    }
    return lua;
}

void GraphCompiler::collect_pure(u32 uid, std::vector<u32>& order, std::unordered_set<u32>& seen,
                                 std::unordered_set<u32>& visiting) {
    const NodeDef* d = def(uid);
    if (!d) return;
    for (const PinDef& p : d->id == "std.graph.return" ? g_.outputs : d->inputs) {
        auto link = data_in_.find({uid, p.id});
        if (link == data_in_.end()) continue;
        const u32 src = link->second.node;
        const NodeDef* sd = def(src);
        if (!sd || sd->kind != NodeKind::Pure || seen.count(src)) continue;
        if (visiting.count(src)) {
            error(src, {}, "провода данных замкнуты в круг");
            continue;
        }
        visiting.insert(src);
        collect_pure(src, order, seen, visiting);
        visiting.erase(src);
        seen.insert(src);
        order.push_back(src);
    }
}

// One line of a template with {in:…}, {out:…}, {self}, {uid}, {vardefault} filled in.
std::string GraphCompiler::expand(u32 uid, const std::string& t) {
    const NodeDef* d = def(uid);
    std::string out;
    usize i = 0;
    while (i < t.size()) {
        if (t[i] != '{') {
            out += t[i++];
            continue;
        }
        const usize close = t.find('}', i);
        if (close == std::string::npos) {
            out += t.substr(i);
            break;
        }
        const std::string ph = t.substr(i + 1, close - i - 1);
        i = close + 1;
        if (ph.rfind("in:", 0) == 0) {
            const PinDef* p = d->input(ph.substr(3));
            if (p) out += input_expr(uid, *p);
            else error(uid, ph.substr(3), "шаблон ноды «" + d->title.get() + "» ссылается на несуществующий вход");
        } else if (ph.rfind("out:", 0) == 0) {
            out += out_var(uid, ph.substr(4));
        } else if (ph == "self") {
            out += "self";
        } else if (ph == "uid") {
            out += std::to_string(uid);
        } else if (ph == "vardefault") {
            const std::string* name = nodes_.at(uid)->value("name");
            const GraphVar* var = name ? g_.variable(*name) : nullptr;
            std::string lua = "nil";
            if (var && !value_to_lua(var->type, var->value, lua)) lua = "nil";
            out += lua;
        } else {
            out += "{" + ph + "}"; // a Luau table constructor, not ours
        }
    }
    return out;
}

// "{in:a} + {in:b}" with its placeholders filled in. Lines that are only a
// {slot:x} or {body} placeholder become nested code.
void GraphCompiler::emit_template(u32 uid, const std::string& tmpl, int ind, u32 chain_after) {
    usize start = 0;
    while (start <= tmpl.size()) {
        usize end = tmpl.find('\n', start);
        if (end == std::string::npos) end = tmpl.size();
        std::string raw = tmpl.substr(start, end - start);
        start = end + 1;
        usize lead = 0;
        while (lead < raw.size() && raw[lead] == ' ') ++lead;
        const int extra = static_cast<int>(lead / 4);
        const std::string t = trim(raw);
        if (t.rfind("{slot:", 0) == 0 && t.back() == '}' && t.find('}') == t.size() - 1) {
            emit_slot(uid, t.substr(6, t.size() - 7), ind + extra);
            continue;
        }
        if (t == "{pure}") { // loops: recompute the inputs on every pass
            std::vector<u32> pure;
            std::unordered_set<u32> seen, visiting;
            collect_pure(uid, pure, seen, visiting);
            for (u32 p : pure) emit_pure(p, ind + extra);
            em_.node = inner_ ? SourceMap::kNoNode : uid;
            continue;
        }
        if (t == "{body}") {
            emit_chain(chain_after, ind + extra);
            continue;
        }
        const std::string out = expand(uid, t);
        if (!out.empty()) em_.line(ind + extra, out);
    }
}

void GraphCompiler::emit_pure(u32 uid, int ind) {
    const NodeDef* d = def(uid);
    const u32 saved = em_.node;
    em_.node = inner_ ? SourceMap::kNoNode : uid;
    std::string names;
    for (const PinDef& p : d->outputs) names += (names.empty() ? "" : ", ") + out_var(uid, p.id);
    std::string args;
    auto collect_args = [&]() {
        args = d->graph.empty() ? "" : "self";
        for (const PinDef& p : d->inputs) args += (args.empty() ? "" : ", ") + input_expr(uid, p);
    };
    const bool timed = opt_.profile && !inner_ && d->graph.empty();
    if (timed) em_.line(ind, "forge.prof.enter(" + std::to_string(uid) + ")");
    if (!d->call.empty()) {
        collect_args();
        em_.line(ind, "local " + names + " = forge." + d->call + "(" + args + ")");
    } else if (!d->graph.empty()) {
        collect_args();
        em_.line(ind, "local " + names + " = " + macro_name(*d) + "(" + args + ")");
    } else if (d->lua.find("{out:") != std::string::npos) {
        if (!names.empty()) em_.line(ind, "local " + names);
        emit_template(uid, d->lua, ind, 0);
    } else if (!d->outputs.empty()) {
        // An expression: fill it in on one line.
        std::string flat = d->lua;
        std::replace(flat.begin(), flat.end(), '\n', ' ');
        em_.line(ind, "local " + out_var(uid, d->outputs[0].id) + " = " + expand(uid, trim(flat)));
    }
    if (timed) em_.line(ind, "forge.prof.leave(" + std::to_string(uid) + ")");
    em_.node = saved;
}

void GraphCompiler::emit_node_code(u32 uid, int ind) {
    const NodeDef* d = def(uid);
    if (d->kind == NodeKind::Comment) return;
    std::string outs;
    for (const PinDef& p : d->outputs) outs += (outs.empty() ? "" : ", ") + out_var(uid, p.id);
    // Blocks that may wait (latent ones, graph nodes) are not timed: a wait
    // would count other scripts' time.
    const bool timed = opt_.profile && !inner_ && d->kind == NodeKind::Action && !d->latent && d->graph.empty();
    if (timed) em_.line(ind, "forge.prof.enter(" + std::to_string(uid) + ")");
    if (d->id == "std.graph.return") {
        std::string vals;
        for (const PinDef& p : g_.outputs) {
            PinDef pin = p;
            pin.required = false;
            vals += (vals.empty() ? "" : ", ") + input_expr(uid, pin);
        }
        em_.line(ind, vals.empty() ? "do return end" : "do return " + vals + " end");
    } else if (!d->call.empty() || !d->graph.empty()) {
        std::string args = d->graph.empty() ? "" : "self";
        for (const PinDef& p : d->inputs) args += (args.empty() ? "" : ", ") + input_expr(uid, p);
        const std::string fn = d->graph.empty() ? "forge." + d->call : macro_name(*d);
        em_.line(ind, (outs.empty() ? "" : outs + " = ") + fn + "(" + args + ")");
    } else {
        emit_template(uid, d->lua, ind, 0);
    }
    if (timed) em_.line(ind, "forge.prof.leave(" + std::to_string(uid) + ")");
}

void GraphCompiler::emit_statement(u32 uid, int ind) {
    const NodeDef* d = def(uid);
    if (!d) return;
    const u32 saved = em_.node;
    em_.node = inner_ ? SourceMap::kNoNode : uid;
    std::vector<u32> pure;
    std::unordered_set<u32> seen, visiting;
    if (d->lua.find("{pure}") == std::string::npos) collect_pure(uid, pure, seen, visiting);
    if (pure.empty()) {
        emit_node_code(uid, ind);
    } else {
        // Pure inputs are computed right before the block, every time it runs,
        // in their own scope (Luau allows 200 locals per function).
        em_.line(ind, "do");
        for (u32 p : pure) emit_pure(p, ind + 1);
        em_.node = inner_ ? SourceMap::kNoNode : uid;
        emit_node_code(uid, ind + 1);
        em_.line(ind, "end");
    }
    em_.node = saved;
}

void GraphCompiler::emit_slot(u32 uid, const std::string& slot, int ind) {
    const GraphNode* n = nodes_.at(uid);
    if (const SlotContent* s = n->slot(slot))
        for (u32 child : s->nodes) {
            if (flow_target(child, kFlowNext))
                warning(child, "у блока в слоте провод «Далее» не используется: блоки слота идут по порядку");
            emit_statement(child, ind);
        }
    if (const u32 next = flow_target(uid, slot)) emit_chain(next, ind);
}

// A node and what follows it through "next" wires.
void GraphCompiler::emit_chain(u32 uid, int ind, u32 start_fn) {
    while (uid) {
        if (chain_fns_.count(uid) && uid != start_fn) {
            const u32 saved = em_.node;
            em_.node = inner_ ? SourceMap::kNoNode : uid;
            em_.line(ind, "c" + std::to_string(uid) + "()");
            em_.node = saved;
            return;
        }
        start_fn = 0;
        emit_statement(uid, ind);
        uid = flow_target(uid, kFlowNext);
    }
}

// Every node a handler can run: for its local outputs and chain functions.
void GraphCompiler::reach(u32 uid, std::vector<u32>& order, std::unordered_set<u32>& seen) {
    std::vector<u32> stack{uid};
    while (!stack.empty()) {
        const u32 u = stack.back();
        stack.pop_back();
        if (!u || seen.count(u)) continue;
        seen.insert(u);
        order.push_back(u);
        const NodeDef* d = def(u);
        if (!d) continue;
        stack.push_back(flow_target(u, kFlowNext));
        const GraphNode* n = nodes_.at(u);
        for (const SlotDef& s : d->slots) {
            stack.push_back(flow_target(u, s.id));
            if (const SlotContent* c = n->slot(s.id))
                for (u32 child : c->nodes) stack.push_back(child);
        }
    }
}

void GraphCompiler::emit_function_body(u32 first, int ind) {
    std::vector<u32> nodes;
    std::unordered_set<u32> seen;
    reach(first, nodes, seen);
    std::sort(nodes.begin(), nodes.end());
    std::vector<std::string> locals;
    std::vector<u32> fns;
    for (u32 u : nodes) {
        const NodeDef* d = def(u);
        if (!d || d->kind == NodeKind::Event) continue;
        if (d->kind != NodeKind::Pure)
            for (const PinDef& p : d->outputs) locals.push_back(out_var(u, p.id));
        if (chain_fns_.count(u)) fns.push_back(u);
    }
    for (u32 u : fns) locals.push_back("c" + std::to_string(u));
    if (locals.size() > 150) {
        error(first, {}, "слишком много нод в одной цепочке: вынесите часть в ноду-граф");
        return;
    }
    for (usize i = 0; i < locals.size(); i += 8) {
        std::string line = "local ";
        for (usize k = i; k < std::min(locals.size(), i + 8); ++k) line += (k > i ? ", " : "") + locals[k];
        em_.line(ind, line);
    }
    for (u32 u : fns) {
        const u32 saved = em_.node;
        em_.node = inner_ ? SourceMap::kNoNode : u;
        em_.line(ind, "c" + std::to_string(u) + " = function()");
        emit_chain(u, ind + 1, u);
        em_.line(ind, "end");
        em_.node = saved;
    }
}

void GraphCompiler::emit_macros() {
    for (const GraphNode& n : g_.nodes) {
        const NodeDef* d = def(n.uid);
        if (!d || d->graph.empty()) continue;
        if (std::find(macro_stack_.begin(), macro_stack_.end(), d->graph) != macro_stack_.end()) {
            error(n.uid, {}, "нода-граф «" + d->title.get() + "» вызывает сама себя");
            continue;
        }
        if (macros_done_.count(d->graph)) continue;
        const Graph* body = lib_.graph(d->graph);
        if (!body) {
            error(n.uid, {}, "граф «" + d->graph + "» для ноды «" + d->title.get() + "» не загружен");
            continue;
        }
        macros_done_.insert(d->graph);
        macro_stack_.push_back(d->graph);
        GraphCompiler inner(*body, lib_, opt_, em_, diags_, macros_done_, macro_stack_, true);
        if (inner.index()) {
            inner.emit_macros();
            inner.emit_macro_body(macro_name(*d));
        }
        macro_stack_.pop_back();
    }
}

void GraphCompiler::emit_macro_body(const std::string& fname) {
    std::string params = "self";
    for (const PinDef& p : g_.inputs) params += ", i_" + ident_part(p.id);
    const u32 saved = em_.node;
    em_.node = SourceMap::kNoNode;
    em_.line(0, "local function " + fname + "(" + params + ")");
    u32 entry = 0, ret = 0;
    for (const GraphNode& n : g_.nodes) {
        if (n.def == "std.graph.entry") entry = n.uid;
        if (n.def == "std.graph.return") ret = n.uid;
    }
    if (const u32 first = entry ? flow_target(entry, kFlowNext) : 0) {
        emit_function_body(first, 1);
        emit_chain(first, 1);
    } else if (ret) {
        emit_statement(ret, 1); // a pure graph: only the result
    } else {
        error(0, {}, "нет ноды «Вход» с проводом «Далее» и нет ноды «Результат»");
    }
    em_.line(0, "end");
    em_.node = saved;
}

void GraphCompiler::emit_handlers() {
    std::vector<u32> events;
    for (const GraphNode& n : g_.nodes)
        if (const NodeDef* d = def(n.uid); d && d->kind == NodeKind::Event) events.push_back(n.uid);
    std::sort(events.begin(), events.end());
    if (events.empty()) warning(0, "в графе нет ни одного события: он ничего не сделает");

    std::set<std::string> lists;
    for (u32 e : events) {
        const NodeDef* d = def(e);
        if (d->event.empty()) {
            error(e, {}, "у события «" + d->title.get() + "» не указан обработчик");
            continue;
        }
        if (!is_ident(d->event)) {
            error(e, {}, "обработчик «" + d->event + "» не годится как имя");
            continue;
        }
        em_.node = SourceMap::kNoNode;
        if (lists.insert(d->event).second) em_.line(0, "S." + d->event + " = {}");
        std::string params = "self";
        for (const PinDef& p : d->outputs) params += ", " + out_var(e, p.id);
        em_.node = e;
        em_.line(0, "S." + d->event + "[#S." + d->event + " + 1] = function(" + params + ")");
        const u32 first = flow_target(e, kFlowNext);
        if (first) emit_function_body(first, 1);
        if (!d->lua.empty()) emit_template(e, d->lua, 1, first);
        else emit_chain(first, 1);
        em_.node = e;
        em_.line(0, "end");
    }

    // Blocks no event leads to never run.
    std::unordered_set<u32> seen;
    std::vector<u32> order;
    for (u32 e : events) reach(flow_target(e, kFlowNext), order, seen);
    for (const GraphNode& n : g_.nodes) {
        const NodeDef* d = def(n.uid);
        if (d && d->has_flow_in() && !seen.count(n.uid))
            warning(n.uid, "нода «" + d->title.get() + "» не подключена к событию и никогда не выполнится");
    }
}

} // namespace

CompileResult compile(const Graph& graph, const NodeLibrary& library, const CompileOptions& options) {
    CompileResult r;
    Emitter em;
    std::set<std::string> done;
    std::vector<std::string> stack{graph.name};
    GraphCompiler c(graph, library, options, em, r.diagnostics, done, stack, false);
    em.line(0, "-- Graph \"" + graph.name + "\", compiled by Forge. Change the graph, not this text.");
    em.line(0, "local forge = forge");
    em.line(0, "local S = {}");
    if (c.index()) {
        c.emit_macros();
        c.emit_handlers();
    }
    em.node = SourceMap::kNoNode;
    em.line(0, "return S");
    r.source = std::move(em.text);
    r.map.line_node = std::move(em.line_node);
    r.ok = std::none_of(r.diagnostics.begin(), r.diagnostics.end(), [](const Diagnostic& d) { return d.error; });
    return r;
}

} // namespace forge::script
