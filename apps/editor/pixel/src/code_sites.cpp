#include "forge/pixel/code_sites.h"

#include <Luau/Ast.h>
#include <Luau/Cst.h>
#include <Luau/ParseOptions.h>
#include <Luau/ParseResult.h>
#include <Luau/Parser.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace forge::pixel {

namespace {

// Luau's positions are lines and byte columns, both from 0.
struct Lines {
    std::vector<u32> starts;
    explicit Lines(std::string_view s) {
        starts.push_back(0);
        for (u32 i = 0; i < s.size(); ++i)
            if (s[i] == '\n') starts.push_back(i + 1);
    }
    u32 at(Luau::Position p) const { return p.line < starts.size() ? starts[p.line] + p.column : starts.back(); }
};

bool is_name(std::string_view s) {
    if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
    for (char c : s)
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    static const char* const kKeywords[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
                                           "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
                                           "true", "until", "while", "continue"};
    for (const char* k : kKeywords)
        if (s == k) return false;
    return true;
}

std::string_view key_text(const Luau::AstExprTable::Item& item) {
    if (item.kind == Luau::AstExprTable::Item::Kind::List || !item.key) return {};
    if (auto* s = item.key->as<Luau::AstExprConstantString>()) return {s->value.data, s->value.size};
    return {};
}

struct Builder {
    std::string_view src;
    Lines lines;
    const Luau::CstNodeMap& cst;

    template <typename T>
    T* cst_of(Luau::AstNode* node) const {
        if (Luau::CstNode* const* n = cst.find(node)) return (*n)->as<T>();
        return nullptr;
    }

    CodeSpan span(const Luau::Location& l) const { return {lines.at(l.begin), lines.at(l.end)}; }

    CodeArg arg(Luau::AstExpr* e) const {
        CodeArg a;
        a.span = span(e->location);
        if (auto* n = e->as<Luau::AstExprConstantNumber>()) {
            a.kind = ArgKind::Number;
            a.number = n->value;
        } else if (auto* i = e->as<Luau::AstExprConstantInteger>()) {
            a.kind = ArgKind::Number;
            a.number = static_cast<f64>(i->value);
        } else if (auto* u = e->as<Luau::AstExprUnary>(); u && u->op == Luau::AstExprUnary::Op::Minus &&
                                                             u->expr->is<Luau::AstExprConstantNumber>()) {
            a.kind = ArgKind::Number;
            a.number = -u->expr->as<Luau::AstExprConstantNumber>()->value;
        } else if (auto* s = e->as<Luau::AstExprConstantString>()) {
            a.kind = ArgKind::String;
            a.text.assign(s->value.data, s->value.size);
        } else if (auto* t = e->as<Luau::AstExprTable>()) {
            a.kind = ArgKind::Table;
            table(t, a);
        } else if (e->is<Luau::AstExprFunction>()) {
            a.kind = ArgKind::Function;
        } else {
            a.kind = ArgKind::Other;
            if (auto* b = e->as<Luau::AstExprBinary>()) {
                using Op = Luau::AstExprBinary::Op;
                if ((b->op == Op::Add || b->op == Op::Sub) && b->right->is<Luau::AstExprConstantNumber>()) {
                    if (auto* op = cst_of<Luau::CstExprOp>(b)) {
                        a.tail = true;
                        a.tail_minus = b->op == Op::Sub;
                        a.tail_op = lines.at(op->opPosition);
                        a.tail_span = span(b->right->location);
                        a.tail_number = b->right->as<Luau::AstExprConstantNumber>()->value;
                    }
                }
                a.loose = b->op == Op::Concat || b->op == Op::CompareNe || b->op == Op::CompareEq ||
                          b->op == Op::CompareLt || b->op == Op::CompareLe || b->op == Op::CompareGt ||
                          b->op == Op::CompareGe || b->op == Op::And || b->op == Op::Or;
            } else if (e->is<Luau::AstExprIfElse>() || e->is<Luau::AstExprTypeAssertion>()) {
                a.loose = true;
            }
        }
        return a;
    }

    void table(Luau::AstExprTable* t, CodeArg& a) const {
        a.close = a.span.end - 1;
        a.last_begin = a.last_end = a.span.begin + 1;
        a.items = static_cast<u32>(t->items.size);
        auto* ct = cst_of<Luau::CstExprTable>(t);
        for (usize i = 0; i < t->items.size; ++i) {
            const auto& item = t->items.data[i];
            const u32 begin = lines.at(item.key ? item.key->location.begin : item.value->location.begin);
            // A [key] item starts at its "[", which the key's location leaves out.
            const u32 key = ct && i < ct->items.size && item.kind == Luau::AstExprTable::Item::Kind::General
                                ? lines.at(ct->items.data[i].indexerOpenPosition)
                                : begin;
            a.last_begin = key;
            a.last_end = lines.at(item.value->location.end);
            a.last_sep = -1;
            if (ct && i < ct->items.size && ct->items.data[i].separator != Luau::CstExprTable::Separator::Missing)
                a.last_sep = static_cast<i32>(lines.at(ct->items.data[i].separatorPosition));
            const std::string_view k = key_text(item);
            if (!k.empty()) a.fields.push_back({std::string(k), key, arg(item.value)});
        }
    }
};

struct Walker : Luau::AstVisitor {
    const Builder& b;
    std::vector<CallSite>& out;
    Walker(const Builder& builder, std::vector<CallSite>& sites) : b(builder), out(sites) {}

    bool visit(Luau::AstExprCall* call) override {
        CallSite s;
        s.span = b.span(call->location);
        const u32 args_at = b.lines.at(call->argLocation.begin);
        s.parens = args_at > 0 && b.src[args_at - 1] == '(';
        if (s.parens) {
            s.open = args_at - 1;
            s.close = b.lines.at(call->argLocation.end) - 1;
        }
        if (call->self) {
            auto* index = call->func->as<Luau::AstExprIndexName>();
            s.method = true;
            s.name = index->index.value;
            s.callee_end = b.lines.at(index->expr->location.end);
            s.line = index->indexLocation.begin.line + 1;
        } else {
            if (auto* g = call->func->as<Luau::AstExprGlobal>()) s.name = g->name.value;
            else if (auto* l = call->func->as<Luau::AstExprLocal>()) s.name = l->local->name.value;
            else if (auto* i = call->func->as<Luau::AstExprIndexName>()) s.name = i->index.value;
            s.callee_end = b.lines.at(call->func->location.end);
            s.line = call->func->location.end.line + 1;
        }
        for (usize i = 0; i < call->args.size; ++i) s.args.push_back(b.arg(call->args.data[i]));
        out.push_back(std::move(s));
        return true;
    }
};

// The line of a table's item: its key's, else its value's (1-based).
u32 item_line(const Luau::AstExprTable::Item& item) { return (item.key ? item.key->location : item.value->location).begin.line + 1; }

// return { clips = { name = function(p) … end } } at the top of the chunk; the lines of the module's keys and of the
// items of its drawings (part 4д).
void find_clips(const Builder& b, Luau::AstStatBlock* root, std::vector<ClipFunction>& out,
                std::vector<std::pair<std::string, u32>>& keys, std::vector<std::pair<std::string, u32>>& drawings) {
    Luau::AstExprTable* module = nullptr;
    for (usize i = 0; i < root->body.size; ++i)
        if (auto* r = root->body.data[i]->as<Luau::AstStatReturn>(); r && r->list.size == 1)
            module = r->list.data[0]->as<Luau::AstExprTable>();
    if (!module) return;
    for (usize i = 0; i < module->items.size; ++i) {
        const auto& item = module->items.data[i];
        if (const std::string_view k = key_text(item); !k.empty()) keys.emplace_back(std::string(k), item_line(item));
        if (auto* d = key_text(item) == "drawings" ? item.value->as<Luau::AstExprTable>() : nullptr)
            for (usize j = 0; j < d->items.size; ++j)
                if (const std::string_view k = key_text(d->items.data[j]); !k.empty()) drawings.emplace_back(std::string(k), item_line(d->items.data[j]));
        auto* clips = key_text(item) == "clips" ? item.value->as<Luau::AstExprTable>() : nullptr;
        if (!clips) continue;
        for (usize j = 0; j < clips->items.size; ++j) {
            const auto& c = clips->items.data[j];
            auto* fn = c.value->as<Luau::AstExprFunction>();
            const std::string_view name = key_text(c);
            if (!fn || name.empty()) continue;
            ClipFunction f;
            f.name = name;
            Luau::Position after = fn->argLocation ? fn->argLocation->end : fn->location.begin;
            if (fn->returnAnnotation) after = fn->returnAnnotation->location.end;
            f.params_end = b.lines.at(after);
            f.header_line = after.line + 1;
            if (fn->body->body.size > 0) {
                const Luau::Position first = fn->body->body.data[0]->location.begin;
                f.first = static_cast<i32>(b.lines.at(first));
                f.first_line = first.line + 1;
            }
            out.push_back(std::move(f));
        }
    }
}

struct Edit {
    u32 at = 0, end = 0; // replaces [at, end) with text
    std::string text;
    u8 kind = 1;         // at one place: closing ")" (0), then replacements (1), then openings (2)
    u32 seq = 0;         // the call's place in sites()
};

std::string apply_edits(std::string_view src, std::vector<Edit>& edits) {
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
        if (a.at != b.at) return a.at < b.at;
        if (a.kind != b.kind) return a.kind < b.kind;
        // The outer call opens first and closes last.
        return a.kind == 0 ? a.seq > b.seq : a.seq < b.seq;
    });
    std::string out;
    out.reserve(src.size() + edits.size() * 16);
    u32 pos = 0;
    for (const Edit& e : edits) {
        if (e.at > pos) out.append(src.substr(pos, e.at - pos));
        out += e.text;
        pos = std::max(pos, e.end);
    }
    out.append(src.substr(std::min<usize>(pos, src.size())));
    return out;
}

// The text that replaces [a, b): its newlines first, so whatever follows stays on its line.
std::string keeping_lines(std::string_view src, u32 a, u32 b, std::string_view text) {
    std::string s(static_cast<usize>(std::count(src.begin() + a, src.begin() + b, '\n')), '\n');
    s += text;
    return s;
}

std::string indent_of(std::string_view src, u32 at) {
    u32 line = at;
    while (line > 0 && src[line - 1] != '\n') --line;
    u32 end = line;
    while (end < src.size() && (src[end] == ' ' || src[end] == '\t')) ++end;
    return std::string(src.substr(line, end - line));
}

bool same(f64 a, f64 b) { return format_number(a) == format_number(b); }

} // namespace

bool CodeSites::parse(std::string_view source, std::string& why, u32& why_line) {
    source_.assign(source);
    sites_.clear();
    clips_.clear();
    keys_.clear();
    drawings_.clear();
    why.clear();
    why_line = 0;
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions options;
    options.storeCstData = true;
    Luau::ParseResult result = Luau::Parser::parse(source_.data(), source_.size(), names, allocator, options);
    if (!result.errors.empty() || !result.root) {
        if (!result.errors.empty()) {
            why = result.errors.front().getMessage();
            why_line = result.errors.front().getLocation().begin.line + 1;
        } else {
            why = "код не разбирается";
        }
        return false;
    }
    const Builder b{source_, Lines(source_), result.cstNodeMap};
    Walker walker(b, sites_);
    result.root->visit(&walker);
    find_clips(b, result.root, clips_, keys_, drawings_);
    return true;
}

const ClipFunction* CodeSites::clip(std::string_view name) const {
    for (const ClipFunction& c : clips_)
        if (c.name == name) return &c;
    return nullptr;
}

u32 CodeSites::module_line(std::string_view key) const {
    for (const auto& [k, line] : keys_)
        if (k == key) return line;
    return 0;
}

u32 CodeSites::drawing_line(std::string_view name) const {
    for (const auto& [k, line] : drawings_)
        if (k == name) return line;
    return 0;
}

std::string CodeSites::instrumented(u32 first_id) const {
    std::vector<Edit> edits;
    edits.reserve(sites_.size() * 3);
    for (u32 i = 0; i < sites_.size(); ++i) {
        const CallSite& s = sites_[i];
        const std::string id = std::to_string(first_id + i);
        edits.push_back({s.span.begin, s.span.begin, (s.method ? "__fm(" : "__fc(") + id + ", ", 2, i});
        std::string between = s.method ? ", " + quote_string(s.name) : std::string();
        if (s.parens) {
            if (!s.args.empty()) between += ", ";
            edits.push_back({s.callee_end, s.open + 1, keeping_lines(source_, s.callee_end, s.open + 1, between), 1, i});
        } else {
            const u32 arg = s.args.front().span.begin;
            edits.push_back({s.callee_end, arg, keeping_lines(source_, s.callee_end, arg, between + ", "), 1, i});
            edits.push_back({s.args.front().span.end, s.args.front().span.end, ")", 0, i});
        }
    }
    return apply_edits(source_, edits);
}

std::string format_number(f64 v) {
    if (!std::isfinite(v)) return "0";
    const f64 r = std::round(v * 1000) / 1000;
    if (r == 0) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", r);
    std::string s = buf;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

std::string quote_string(std::string_view text) {
    std::string s = "\"";
    for (char c : text) {
        switch (c) {
        case '"': s += "\\\""; break;
        case '\\': s += "\\\\"; break;
        case '\n': s += "\\n"; break;
        case '\r': s += "\\r"; break;
        case '\t': s += "\\t"; break;
        default:
            if (static_cast<u8>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\%03u", static_cast<unsigned>(static_cast<u8>(c)));
                s += buf;
            } else {
                s += c;
            }
        }
    }
    return s + '"';
}

bool rewrite_number(std::string_view source, const CodeArg& arg, f64 old_value, f64 value, std::string& out,
                    std::string& why) {
    out.clear();
    why.clear();
    if (!std::isfinite(value)) return why = "новое значение — не число", false;
    if (arg.span.end > source.size()) return why = "код изменился: место вызова устарело", false;
    std::vector<Edit> edits;
    if (arg.kind == ArgKind::Number) {
        edits.push_back({arg.span.begin, arg.span.end, format_number(value)});
    } else if (arg.kind == ArgKind::Other) {
        if (same(value, old_value)) {
            out.assign(source);
            return true;
        }
        const f64 delta = value - old_value;
        if (arg.tail) {
            const f64 c = (arg.tail_minus ? -arg.tail_number : arg.tail_number) + delta;
            edits.push_back({arg.tail_op, arg.tail_op + 1, c < 0 ? "-" : "+"});
            edits.push_back({arg.tail_span.begin, arg.tail_span.end, format_number(std::fabs(c))});
        } else {
            const std::string fix = (delta < 0 ? " - " : " + ") + format_number(std::fabs(delta));
            if (arg.loose) {
                edits.push_back({arg.span.begin, arg.span.begin, "("});
                edits.push_back({arg.span.end, arg.span.end, ")" + fix});
            } else {
                edits.push_back({arg.span.end, arg.span.end, fix});
            }
        }
    } else {
        return why = "там не число: строка, таблица или функция", false;
    }
    out = apply_edits(source, edits);
    return true;
}

bool rewrite_arg(std::string_view source, const CallSite& site, usize index, std::string_view text, std::string& out,
                 std::string& why) {
    out.clear();
    why.clear();
    if (site.span.end > source.size()) return why = "код изменился: место вызова устарело", false;
    std::vector<Edit> edits;
    if (index < site.args.size()) {
        edits.push_back({site.args[index].span.begin, site.args[index].span.end, std::string(text)});
    } else if (index == site.args.size()) {
        if (!site.parens) {
            // f{…} and f"…": f({…}, text)
            const CodeSpan a = site.args.front().span;
            edits.push_back({a.begin, a.begin, "("});
            edits.push_back({a.end, a.end, ", " + std::string(text) + ")"});
        } else if (site.args.empty()) {
            edits.push_back({site.open + 1, site.open + 1, std::string(text)});
        } else {
            const u32 end = site.args.back().span.end;
            edits.push_back({end, end, ", " + std::string(text)});
        }
    } else {
        return why = "у вызова меньше аргументов, чем нужно до этого", false;
    }
    out = apply_edits(source, edits);
    return true;
}

bool rewrite_field(std::string_view source, const CodeArg& table, std::string_view name, std::string_view text,
                   std::string& out, std::string& why) {
    out.clear();
    why.clear();
    if (table.kind != ArgKind::Table) return why = "там не таблица", false;
    if (table.span.end > source.size()) return why = "код изменился: место вызова устарело", false;
    for (const CodeField& f : table.fields)
        if (f.name == name) {
            std::vector<Edit> edits;
            edits.push_back({f.value.span.begin, f.value.span.end, std::string(text)});
            out = apply_edits(source, edits);
            return true;
        }
    const std::string field = (is_name(name) ? std::string(name) : "[" + quote_string(name) + "]") + " = " +
                              std::string(text);
    std::vector<Edit> edits;
    if (table.items == 0) {
        edits.push_back({table.last_end, table.last_end, field});
    } else {
        const u32 after = table.last_sep >= 0 ? static_cast<u32>(table.last_sep) + 1 : table.last_end;
        const char* nl = static_cast<const char*>(std::memchr(source.data() + after, '\n', table.close - after));
        if (nl) {
            // One field a line: a new line like the last, its comment left where it is.
            const u32 line_end = static_cast<u32>(nl - source.data());
            const std::string sep = table.last_sep >= 0 ? std::string(1, source[static_cast<u32>(table.last_sep)]) : "";
            if (table.last_sep < 0) edits.push_back({table.last_end, table.last_end, ","});
            edits.push_back({line_end, line_end, "\n" + indent_of(source, table.last_begin) + field + sep});
        } else {
            edits.push_back({table.last_end, table.last_end, ", " + field});
        }
    }
    out = apply_edits(source, edits);
    return true;
}

bool rewrite_field_number(std::string_view source, const CodeArg& table, std::string_view name, f64 old_value,
                          f64 value, std::string& out, std::string& why) {
    if (table.kind == ArgKind::Table)
        for (const CodeField& f : table.fields)
            if (f.name == name) return rewrite_number(source, f.value, old_value, value, out, why);
    if (!std::isfinite(value)) {
        out.clear();
        return why = "новое значение — не число", false;
    }
    return rewrite_field(source, table, name, format_number(value), out, why);
}

bool insert_clip_line(const CodeSites& code, std::string_view source, std::string_view clip, std::string_view line,
                      std::string& out, std::string& why) {
    out.clear();
    why.clear();
    const ClipFunction* c = code.clip(clip);
    if (!c) {
        why = "в коде нет функции клипа «" + std::string(clip) + "» в таблице clips: строку не вставить";
        return false;
    }
    if (c->params_end > source.size()) return why = "код изменился: место вызова устарело", false;
    std::vector<Edit> edits;
    if (c->first >= 0 && c->first_line > c->header_line) {
        u32 at = static_cast<u32>(c->first);
        while (at > 0 && source[at - 1] != '\n') --at;
        // Ended as the line above it is: a file of CRLF lines stays one.
        const char* end = at >= 2 && source[at - 2] == '\r' ? "\r\n" : "\n";
        edits.push_back({at, at, indent_of(source, static_cast<u32>(c->first)) + std::string(line) + end});
    } else {
        edits.push_back({c->params_end, c->params_end, " " + std::string(line)});
    }
    out = apply_edits(source, edits);
    return true;
}

} // namespace forge::pixel
