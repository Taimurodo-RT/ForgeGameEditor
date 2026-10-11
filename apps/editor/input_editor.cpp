#include "input_editor.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <algorithm>

namespace forge::editor_app {

// The author's choices before and after one change of the tab, written to input.json both ways.
class InputCommand final : public editor::Command {
public:
    InputCommand(InputEditor& tab, input::Choices before, input::Choices after, std::string label)
        : tab_(tab), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
    void apply(editor::Document&) override { tab_.put(after_); }
    void revert(editor::Document&) override { tab_.put(before_); }
    std::string label() const override { return label_; }

private:
    InputEditor& tab_;
    input::Choices before_, after_;
    std::string label_;
};

namespace {

std::vector<std::string> texts(const std::vector<input::Bind>& binds) {
    std::vector<std::string> out;
    for (const input::Bind& b : binds) out.push_back(input::format(b));
    return out;
}

// The module's defaults as the file writes binds ("key:space" read is "key:Space").
std::vector<std::string> canonical(const std::vector<std::string>& binds) {
    std::vector<std::string> out;
    for (const std::string& t : binds)
        if (input::Bind b; input::parse(t, b)) {
            const std::string f = input::format(b);
            if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
        }
    return out;
}

} // namespace

void InputEditor::open(const std::filesystem::path& file, std::vector<input::ActionDef> module_actions, std::string module_name) {
    file_ = file;
    module_name_ = std::move(module_name);
    module_count_ = module_actions.size();
    defs_ = std::move(module_actions);
    for (const input::ActionDef& d : input::shell_actions()) defs_.push_back(d);
    choices_.clear();
    file_warnings_.clear();
    error_.clear();
    std::error_code ec;
    if (std::filesystem::exists(file_, ec)) {
        std::vector<u8> bytes;
        std::string why;
        if (!read_file(file_, bytes)) why = "не читается";
        else if (!input::read_choices(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), choices_, &why, &file_warnings_))
            choices_.clear();
        else why.clear();
        if (!why.empty()) {
            error_ = "input.json игры: " + why + ". Вкладка его не меняет, пока он такой: исправьте или удалите файл.";
            FORGE_ERROR("%s", error_.c_str());
        }
    }
    history_.clear();
    rebuild();
}

void InputEditor::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<Chip>()) {
        s.RegisterMember("text", &Chip::text);
        s.RegisterMember("words", &Chip::words);
        s.RegisterMember("index", &Chip::index);
        s.RegisterMember("pad", &Chip::pad);
    }
    model.RegisterArray<std::vector<Chip>>();
    if (auto s = model.RegisterStruct<Row>()) {
        s.RegisterMember("id", &Row::id);
        s.RegisterMember("words", &Row::words);
        s.RegisterMember("binds", &Row::binds);
        s.RegisterMember("changed", &Row::changed);
        s.RegisterMember("waiting", &Row::waiting);
        s.RegisterMember("more", &Row::more);
        s.RegisterMember("empty", &Row::empty);
    }
    model.RegisterArray<std::vector<Row>>();
    if (auto s = model.RegisterStruct<Pick>()) {
        s.RegisterMember("text", &Pick::text);
        s.RegisterMember("words", &Pick::words);
        s.RegisterMember("pad", &Pick::pad);
        s.RegisterMember("has", &Pick::has);
    }
    model.RegisterArray<std::vector<Pick>>();
    model.RegisterArray<std::vector<Rml::String>>();
    model.Bind("in_game", &m_game_);
    model.Bind("in_shell", &m_shell_);
    model.Bind("in_picks", &m_picks_);
    model.Bind("in_warnings", &m_warnings_);
    model.Bind("in_note", &m_note_);
    model.Bind("in_error", &m_error_);
    model.Bind("in_module_note", &m_module_note_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) { fn(args); });
    };
    auto arg_int = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<int>(-1) : -1; };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    on("in_key", [this, arg_str](const Rml::VariantList& a) { wait_key(arg_str(a, 0)); });
    on("in_more", [this, arg_str](const Rml::VariantList& a) { show_more(arg_str(a, 0)); });
    on("in_pick", [this, arg_int](const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        const std::vector<input::Bind> all = input::mouse_and_pad_binds();
        if (!more_.empty() && i >= 0 && i < static_cast<int>(all.size())) add(std::string(more_), all[static_cast<usize>(i)]);
    });
    on("in_remove", [this, arg_str, arg_int](const Rml::VariantList& a) {
        if (const int i = arg_int(a, 1); i >= 0) remove(arg_str(a, 0), static_cast<usize>(i));
    });
    on("in_reset", [this, arg_str](const Rml::VariantList& a) { reset(arg_str(a, 0)); });
}

void InputEditor::set_model(Rml::DataModelHandle handle) {
    model_ = handle;
    rebuild();
}

bool InputEditor::handle_event(const SDL_Event& e, bool shown) {
    pads_.handle_event(e);
    pads_.end_frame(); // only what is connected is kept
    if (waiting_.empty()) return false;
    if (!shown) {
        waiting_.clear();
        rebuild();
        return false;
    }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_ESCAPE) {
        waiting_.clear();
        note_.clear();
        rebuild();
        return true;
    }
    const std::optional<input::Bind> b = input::bind_of(e);
    if (b && b->device != input::Device::Mouse) {
        const std::string action = waiting_;
        waiting_.clear();
        if (!add(action, *b)) rebuild();
        return true;
    }
    // The other keys (a key let go, a repeat) wait with the action; the mouse goes on to the buttons.
    return e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP;
}

void InputEditor::undo() {
    if (!history_.undo()) return;
    FORGE_INFO("Отменено");
}

void InputEditor::redo() {
    if (!history_.redo()) return;
    FORGE_INFO("Повторено");
}

std::string InputEditor::status() const {
    usize changed_n = 0;
    for (const input::ActionDef& d : defs_) changed_n += changed(d.id) ? 1 : 0;
    std::string s = "Ввод: действий " + std::to_string(defs_.size()) + " · автор изменил " + std::to_string(changed_n);
    if (!error_.empty()) return s + " · input.json не читается";
    if (!waiting_.empty())
        if (const input::ActionDef* d = def(waiting_)) return s + " · «" + d->words + "» ждёт клавишу";
    const std::vector<std::string> w = warnings();
    if (!w.empty()) s += " · " + w.front() + (w.size() > 1 ? " (и ещё " + std::to_string(w.size() - 1) + ")" : "");
    return s;
}

// --- the actions ----------------------------------------------------------------

const input::ActionDef* InputEditor::def(std::string_view action) const {
    const auto it = std::find_if(defs_.begin(), defs_.end(), [&](const input::ActionDef& d) { return d.id == action; });
    return it == defs_.end() ? nullptr : &*it;
}

std::vector<std::string> InputEditor::binds(std::string_view action) const {
    for (const input::Action& a : resolved_)
        if (a.id == action) return texts(a.binds);
    return {};
}

bool InputEditor::changed(std::string_view action) const { return input::find(choices_, action) != nullptr; }

bool InputEditor::change(input::Choices to, std::string label) {
    if (!error_.empty()) {
        FORGE_WARN("%s", error_.c_str());
        return false;
    }
    if (to.size() == choices_.size() &&
        std::equal(to.begin(), to.end(), choices_.begin(), [](const input::Choice& a, const input::Choice& b) { return a.action == b.action && a.binds == b.binds; }))
        return false;
    history_.execute(std::make_unique<InputCommand>(*this, choices_, std::move(to), std::move(label)));
    history_.seal();
    return true;
}

void InputEditor::put(const input::Choices& choices) {
    choices_ = choices;
    std::error_code ec;
    if (choices_.empty()) {
        // Nothing changed: no file, as the game was before the tab.
        if (std::filesystem::exists(file_, ec) && !std::filesystem::remove(file_, ec))
            FORGE_ERROR("%s не удалился: %s", path_to_utf8(file_).c_str(), ec.message().c_str());
    } else {
        const std::string text = input::write_choices(choices_);
        if (!write_file_atomic(file_, {reinterpret_cast<const u8*>(text.data()), text.size()}))
            FORGE_ERROR("%s не записался", path_to_utf8(file_).c_str());
    }
    file_warnings_.clear();
    rebuild();
}

bool InputEditor::add(std::string_view action, const input::Bind& bind) {
    const input::ActionDef* d = def(action);
    if (!d) return false;
    std::vector<std::string> now = binds(action);
    const std::string text = input::format(bind);
    note_.clear();
    if (std::find(now.begin(), now.end(), text) != now.end()) {
        note_ = "«" + input::words(bind) + "» уже у «" + d->words + "»";
        rebuild();
        return false;
    }
    now.push_back(text);
    input::Choices to = choices_;
    input::choose(to, action, std::move(now), canonical(d->defaults));
    return change(std::move(to), "«" + d->words + "»: " + input::words(bind));
}

bool InputEditor::remove(std::string_view action, usize index) {
    const input::ActionDef* d = def(action);
    std::vector<std::string> now = binds(action);
    if (!d || index >= now.size()) return false;
    input::Bind b;
    input::parse(now[index], b);
    now.erase(now.begin() + static_cast<std::ptrdiff_t>(index));
    input::Choices to = choices_;
    input::choose(to, action, std::move(now), canonical(d->defaults));
    note_.clear();
    return change(std::move(to), "«" + d->words + "»: без «" + input::words(b) + "»");
}

bool InputEditor::reset(std::string_view action) {
    const input::ActionDef* d = def(action);
    if (!d || !changed(action)) return false;
    input::Choices to = choices_;
    input::choose(to, action, canonical(d->defaults), canonical(d->defaults));
    note_.clear();
    return change(std::move(to), "«" + d->words + "»: как было");
}

void InputEditor::wait_key(std::string_view action) {
    waiting_ = waiting_ == action || !def(action) ? std::string() : std::string(action);
    note_.clear();
    rebuild();
}

void InputEditor::show_more(std::string_view action) {
    more_ = more_ == action || !def(action) ? std::string() : std::string(action);
    rebuild();
}

std::vector<std::string> InputEditor::warnings() const {
    std::vector<std::string> out;
    for (const input::Clash& c : input::clashes(resolved_)) out.push_back(input::clash_words(c));
    for (const input::Action& a : resolved_)
        if (a.binds.empty()) out.push_back("У «" + a.words + "» нет ни одной клавиши: игрок не сможет это сделать");
    for (const std::string& w : file_warnings_) out.push_back(w);
    return out;
}

// --- the views ----------------------------------------------------------------------

void InputEditor::rebuild() {
    std::vector<std::string> resolving;
    resolved_ = input::resolve(defs_, choices_, {}, &resolving);
    // What reading the file said, and what resolving it says now (once each).
    for (std::string& w : resolving)
        if (std::find(file_warnings_.begin(), file_warnings_.end(), w) == file_warnings_.end()) file_warnings_.push_back(std::move(w));
    m_game_.clear();
    m_shell_.clear();
    for (usize i = 0; i < resolved_.size(); ++i) {
        const input::Action& a = resolved_[i];
        Row r{a.id, a.words, {}, changed(a.id), waiting_ == a.id, more_ == a.id, a.binds.empty()};
        for (usize j = 0; j < a.binds.size(); ++j)
            r.binds.push_back({input::format(a.binds[j]), input::words(a.binds[j]), static_cast<int>(j), input::is_pad(a.binds[j])});
        (i < module_count_ ? m_game_ : m_shell_).push_back(std::move(r));
    }
    m_picks_.clear();
    const std::vector<std::string> open = more_.empty() ? std::vector<std::string>{} : binds(more_);
    for (const input::Bind& b : input::mouse_and_pad_binds()) {
        const std::string t = input::format(b);
        m_picks_.push_back({t, input::words(b), input::is_pad(b), std::find(open.begin(), open.end(), t) != open.end()});
    }
    m_warnings_.clear();
    for (const std::string& w : warnings()) m_warnings_.push_back(w);
    m_note_ = note_;
    m_error_ = error_;
    m_module_note_ = module_count_ ? std::string()
                                   : "У модуля «" + module_name_ + "» своих действий нет: его игры не спрашивают клавиш. Действия оболочки ниже есть в каждой игре.";
    for (const char* name : {"in_game", "in_shell", "in_picks", "in_warnings", "in_note", "in_error", "in_module_note"}) dirty(name);
}

} // namespace forge::editor_app
