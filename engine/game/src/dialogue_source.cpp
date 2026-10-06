#include "forge/game/dialogue_source.h"

#include <yyjson.h>

#include <cstdio>

namespace forge::game {

namespace {

std::string str(yyjson_val* v) { return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string(); }

// "do" may be one string or a list of strings.
std::string actions(yyjson_val* v) {
    if (yyjson_is_str(v)) return str(v);
    std::string out;
    yyjson_arr_iter it = yyjson_arr_iter_with(v);
    for (yyjson_val* s; (s = yyjson_arr_iter_next(&it));) {
        if (!out.empty()) out += "; ";
        out += str(s);
    }
    return out;
}

std::vector<SourceEntry> entries(yyjson_val* v) {
    std::vector<SourceEntry> out;
    if (yyjson_is_str(v)) {
        out.push_back({{}, str(v)});
        return out;
    }
    yyjson_arr_iter it = yyjson_arr_iter_with(v);
    for (yyjson_val* e; (e = yyjson_arr_iter_next(&it));) out.push_back({str(yyjson_obj_get(e, "if")), str(yyjson_obj_get(e, "goto"))});
    return out;
}

// A JSON string literal.
std::string q(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': break;
        default:
            if (static_cast<u8>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

void field(std::string& out, const char* name, const std::string& value) {
    if (value.empty()) return;
    out += ", ";
    out += q(name);
    out += ": ";
    out += q(value);
}

std::string entry(const SourceEntry& e) {
    std::string out = "{";
    if (!e.cond.empty()) out += "\"if\": " + q(e.cond) + ", ";
    out += "\"goto\": " + q(e.go.empty() ? "end" : e.go) + "}";
    return out;
}

} // namespace

bool DialogueSource::parse(std::string_view json, std::string* error) {
    *this = {};
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) {
        if (error) *error = "это не JSON";
        return false;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        if (error) *error = "в файле нет разговора";
        return false;
    }
    id = str(yyjson_obj_get(root, "id"));
    if (yyjson_val* sp = yyjson_obj_get(root, "speakers"); yyjson_is_obj(sp)) {
        yyjson_obj_iter it = yyjson_obj_iter_with(sp);
        for (yyjson_val* k; (k = yyjson_obj_iter_next(&it));) {
            yyjson_val* v = yyjson_obj_iter_get_val(k);
            speakers.push_back({str(k), str(yyjson_obj_get(v, "name")), str(yyjson_obj_get(v, "color")), str(yyjson_obj_get(v, "portrait"))});
        }
    }
    start = entries(yyjson_obj_get(root, "start"));
    yyjson_arr_iter nit = yyjson_arr_iter_with(yyjson_obj_get(root, "nodes"));
    for (yyjson_val* n; (n = yyjson_arr_iter_next(&nit));) {
        SourceNode node;
        node.id = str(yyjson_obj_get(n, "id"));
        node.scene = str(yyjson_obj_get(n, "scene"));
        node.note = str(yyjson_obj_get(n, "note"));
        node.speaker = str(yyjson_obj_get(n, "speaker"));
        node.text = str(yyjson_obj_get(n, "text"));
        node.act = actions(yyjson_obj_get(n, "do"));
        node.branches = entries(yyjson_obj_get(n, "branches"));
        node.fallback = str(yyjson_obj_get(n, "fallback"));
        node.next = str(yyjson_obj_get(n, "next"));
        yyjson_arr_iter kit = yyjson_arr_iter_with(yyjson_obj_get(n, "keywords"));
        for (yyjson_val* k; (k = yyjson_arr_iter_next(&kit));) {
            SourceKeyword kw;
            yyjson_arr_iter wit = yyjson_arr_iter_with(yyjson_obj_get(k, "words"));
            for (yyjson_val* w; (w = yyjson_arr_iter_next(&wit));) kw.words.push_back(str(w));
            kw.cond = str(yyjson_obj_get(k, "if"));
            kw.act = actions(yyjson_obj_get(k, "do"));
            kw.go = str(yyjson_obj_get(k, "goto"));
            node.keywords.push_back(std::move(kw));
        }
        yyjson_arr_iter cit = yyjson_arr_iter_with(yyjson_obj_get(n, "choices"));
        for (yyjson_val* c; (c = yyjson_arr_iter_next(&cit));)
            node.choices.push_back({str(yyjson_obj_get(c, "text")), str(yyjson_obj_get(c, "if")), actions(yyjson_obj_get(c, "do")),
                                    str(yyjson_obj_get(c, "goto"))});
        nodes.push_back(std::move(node));
    }
    yyjson_doc_free(doc);
    return true;
}

std::string DialogueSource::json() const {
    std::string out = "{\n  \"id\": " + q(id) + ",\n  \"speakers\": {";
    for (usize i = 0; i < speakers.size(); ++i) {
        const SourceSpeaker& s = speakers[i];
        out += i ? ",\n               " : "";
        out += q(s.key) + ": {\"name\": " + q(s.name);
        field(out, "color", s.color);
        field(out, "portrait", s.portrait);
        out += "}";
    }
    out += "},\n  \"start\": [";
    for (usize i = 0; i < start.size(); ++i) out += (i ? ",\n    " : "\n    ") + entry(start[i]);
    out += start.empty() ? "],\n" : "\n  ],\n";
    out += "  \"nodes\": [";
    for (usize i = 0; i < nodes.size(); ++i) {
        const SourceNode& n = nodes[i];
        out += i ? ",\n" : "\n";
        out += "    {\"id\": " + q(n.id);
        field(out, "scene", n.scene);
        field(out, "speaker", n.speaker);
        if (!n.note.empty()) out += ",\n     \"note\": " + q(n.note);
        if (!n.text.empty()) out += ",\n     \"text\": " + q(n.text);
        if (!n.act.empty()) out += ",\n     \"do\": " + q(n.act);
        if (!n.branches.empty()) {
            out += ",\n     \"branches\": [";
            for (usize j = 0; j < n.branches.size(); ++j) out += (j ? ", " : "") + entry(n.branches[j]);
            out += "]";
        }
        if (!n.keywords.empty()) {
            out += ",\n     \"keywords\": [";
            for (usize j = 0; j < n.keywords.size(); ++j) {
                const SourceKeyword& k = n.keywords[j];
                out += j ? ",\n       {\"words\": [" : "\n       {\"words\": [";
                for (usize w = 0; w < k.words.size(); ++w) out += (w ? ", " : "") + q(k.words[w]);
                out += "]";
                field(out, "if", k.cond);
                field(out, "do", k.act);
                out += ", \"goto\": " + q(k.go.empty() ? "end" : k.go) + "}";
            }
            out += "\n     ]";
        }
        if (!n.fallback.empty()) out += ",\n     \"fallback\": " + q(n.fallback);
        if (!n.choices.empty()) {
            out += ",\n     \"choices\": [";
            for (usize j = 0; j < n.choices.size(); ++j) {
                const SourceChoice& c = n.choices[j];
                out += j ? ",\n       {\"text\": " : "\n       {\"text\": ";
                out += q(c.text);
                field(out, "if", c.cond);
                field(out, "do", c.act);
                out += ", \"goto\": " + q(c.go.empty() ? "end" : c.go) + "}";
            }
            out += "\n     ]";
        }
        if (!n.next.empty()) out += ",\n     \"next\": " + q(n.next);
        out += "}";
    }
    out += nodes.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

SourceNode* DialogueSource::node(std::string_view nid) {
    for (SourceNode& n : nodes)
        if (n.id == nid) return &n;
    return nullptr;
}

const SourceNode* DialogueSource::node(std::string_view nid) const { return const_cast<DialogueSource*>(this)->node(nid); }

const SourceSpeaker* DialogueSource::speaker(std::string_view key) const {
    for (const SourceSpeaker& s : speakers)
        if (s.key == key) return &s;
    return nullptr;
}

i32 DialogueSource::index_of(std::string_view nid) const {
    for (usize i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == nid) return static_cast<i32>(i);
    return -1;
}

std::string DialogueSource::fresh_id(std::string_view stem) const {
    std::string id_(stem);
    for (u32 n = 2; node(id_); ++n) id_ = std::string(stem) + std::to_string(n);
    return id_;
}

} // namespace forge::game
