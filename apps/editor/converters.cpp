#include "converters.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <SDL3/SDL.h>
#include <yyjson.h>

#include <algorithm>
#include <cstdlib>

#ifndef FORGE_PYTHON_EXE
#define FORGE_PYTHON_EXE ""
#endif

namespace fs = std::filesystem;

namespace forge::editor_app {

namespace {

constexpr const char* kRunner = "forge_convert.py";

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string text_of(yyjson_val* v) { return v && yyjson_is_str(v) ? yyjson_get_str(v) : std::string(); }

std::string json_of(yyjson_val* v) {
    if (!v) return {};
    usize len = 0;
    char* s = yyjson_val_write(v, 0, &len);
    std::string out = s ? std::string(s, len) : std::string();
    free(s);
    return out;
}

std::string json_text(const std::string& s) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* v = yyjson_mut_strncpy(doc, s.data(), s.size());
    yyjson_mut_doc_set_root(doc, v);
    usize len = 0;
    char* w = yyjson_mut_write(doc, 0, &len);
    std::string out(w, len);
    free(w);
    yyjson_mut_doc_free(doc);
    return out;
}

// A program on the PATH ("python3"), as a full path; empty when missing.
fs::path on_path(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
#ifdef _WIN32
    const char sep = ';';
    const std::string exe = std::string(name) + ".exe";
#else
    const char sep = ':';
    const std::string exe = name;
#endif
    std::string dirs = path;
    usize at = 0;
    while (at <= dirs.size()) {
        usize end = dirs.find(sep, at);
        if (end == std::string::npos) end = dirs.size();
        if (end > at) {
            const fs::path p = utf8_path(dirs.substr(at, end - at)) / exe;
            std::error_code ec;
            // The Microsoft Store stub is not a Python.
            if (fs::is_regular_file(p, ec) && path_to_utf8(p).find("WindowsApps") == std::string::npos) return p;
        }
        at = end + 1;
    }
    return {};
}

} // namespace

bool ConverterSetting::accepts(const std::string& json) const {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) return false;
    yyjson_val* v = yyjson_doc_get_root(doc);
    bool ok = false;
    if (type == "bool") ok = yyjson_is_bool(v);
    else if (type == "number") ok = yyjson_is_num(v) && yyjson_get_num(v) >= min - 1e-9 && yyjson_get_num(v) <= max + 1e-9;
    else if (type == "text") ok = yyjson_is_str(v);
    else if (type == "choice" && yyjson_is_str(v)) {
        const std::string_view picked(yyjson_get_str(v), yyjson_get_len(v));
        ok = std::any_of(choices.begin(), choices.end(), [&](const ConverterChoice& c) { return c.id == picked; });
    }
    yyjson_doc_free(doc);
    return ok;
}

bool Converter::takes(const std::string& extension) const {
    const std::string e = lower(extension);
    return std::find(from.begin(), from.end(), e) != from.end();
}

Converters::Converters() {
    if (const char* env = std::getenv("FORGE_PYTHON"); env && *env) python_ = utf8_path(env);
    else if (std::error_code ec; *FORGE_PYTHON_EXE && fs::exists(utf8_path(FORGE_PYTHON_EXE), ec)) python_ = utf8_path(FORGE_PYTHON_EXE);
    else if (fs::path p = on_path("python3"); !p.empty()) python_ = p;
    else python_ = on_path("python");
    const u32 n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    for (u32 i = 0; i < n; ++i) workers_.emplace_back([this] { worker_main(); });
}

Converters::~Converters() {
    stop();
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void Converters::load(const std::vector<fs::path>& folders) {
    list_.clear();
    fs::path shared_runner;
    for (const fs::path& folder : folders) {
        std::error_code ec;
        fs::path runner = folder / kRunner;
        if (!fs::is_regular_file(runner, ec)) runner = shared_runner;
        else if (shared_runner.empty()) shared_runner = runner;
        if (runner.empty() || !fs::is_directory(folder, ec)) continue;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            if (path_to_utf8(it->path().filename()).ends_with(".converter.json")) files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            std::vector<u8> bytes;
            if (!read_file(file, bytes)) continue;
            yyjson_doc* doc = yyjson_read_opts(reinterpret_cast<char*>(bytes.data()), bytes.size(),
                                               YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, nullptr, nullptr);
            yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
            if (!yyjson_is_obj(root)) {
                FORGE_WARN("Конвертер %s: описание не читается", path_to_utf8(file.filename()).c_str());
                yyjson_doc_free(doc);
                continue;
            }
            Converter c;
            const std::string fname = path_to_utf8(file.filename());
            c.id = fname.substr(0, fname.size() - std::string(".converter.json").size());
            c.folder = folder;
            c.runner = runner;
            c.name = text_of(yyjson_obj_get(root, "name"));
            c.about = text_of(yyjson_obj_get(root, "about"));
            c.icon = text_of(yyjson_obj_get(root, "icon"));
            if (c.icon.empty()) c.icon = "transform";
            usize i, n;
            yyjson_val* v;
            yyjson_arr_foreach(yyjson_obj_get(root, "from"), i, n, v) {
                std::string e = lower(text_of(v));
                if (!e.empty() && e[0] != '.') e.insert(e.begin(), '.');
                c.from.push_back(e);
            }
            yyjson_arr_foreach(yyjson_obj_get(root, "settings"), i, n, v) {
                ConverterSetting s;
                s.id = text_of(yyjson_obj_get(v, "id"));
                s.name = text_of(yyjson_obj_get(v, "name"));
                s.hint = text_of(yyjson_obj_get(v, "hint"));
                s.type = text_of(yyjson_obj_get(v, "type"));
                s.value = json_of(yyjson_obj_get(v, "default"));
                if (yyjson_val* x = yyjson_obj_get(v, "min")) s.min = yyjson_get_num(x);
                if (yyjson_val* x = yyjson_obj_get(v, "max")) s.max = yyjson_get_num(x);
                if (yyjson_val* x = yyjson_obj_get(v, "step")) s.step = yyjson_get_num(x);
                usize ci, cn;
                yyjson_val* ch;
                yyjson_arr_foreach(yyjson_obj_get(v, "choices"), ci, cn, ch)
                    s.choices.push_back({text_of(yyjson_obj_get(ch, "id")), text_of(yyjson_obj_get(ch, "name"))});
                if (s.value.empty()) {
                    if (s.type == "bool") s.value = "false";
                    else if (s.type == "number") s.value = "0";
                    else if (s.type == "choice" && !s.choices.empty()) s.value = json_text(s.choices[0].id);
                    else s.value = "\"\"";
                }
                if (!s.id.empty()) c.settings.push_back(std::move(s));
            }
            yyjson_doc_free(doc);
            if (c.name.empty() || c.from.empty()) continue;
            // The project's own converter of the same name wins.
            auto same = std::find_if(list_.begin(), list_.end(), [&](const Converter& o) { return o.id == c.id; });
            if (same != list_.end()) *same = std::move(c);
            else list_.push_back(std::move(c));
        }
    }
}

const Converter* Converters::find(const std::string& id) const {
    for (const Converter& c : list_)
        if (c.id == id) return &c;
    return nullptr;
}

u64 Converters::start(const Converter& c, const fs::path& input, const fs::path& out, const std::string& settings) {
    std::lock_guard lock(mutex_);
    if (queue_.empty() && running_ == 0) total_ = done_ = 0;
    const u64 id = next_id_++;
    queue_.push_back({id, &c, input, out, settings.empty() ? "{}" : settings});
    ++total_;
    cv_.notify_one();
    return id;
}

std::vector<Converters::Result> Converters::take_finished() {
    std::lock_guard lock(mutex_);
    std::vector<Result> out;
    out.swap(finished_);
    return out;
}

u32 Converters::running() const {
    std::lock_guard lock(mutex_);
    return running_ + static_cast<u32>(queue_.size());
}

u32 Converters::done() const {
    std::lock_guard lock(mutex_);
    return done_;
}

u32 Converters::total() const {
    std::lock_guard lock(mutex_);
    return total_;
}

f32 Converters::progress() const {
    std::lock_guard lock(mutex_);
    if (total_ == 0) return 1;
    f32 p = static_cast<f32>(done_);
    for (const auto& [id, v] : progress_) p += v;
    return p / static_cast<f32>(total_);
}

std::string Converters::message() const {
    std::lock_guard lock(mutex_);
    return message_;
}

void Converters::stop() {
    std::lock_guard lock(mutex_);
    total_ -= static_cast<u32>(queue_.size());
    queue_.clear();
    for (auto& [id, p] : processes_) SDL_KillProcess(static_cast<SDL_Process*>(p), true);
}

void Converters::worker_main() {
    std::unique_lock lock(mutex_);
    while (true) {
        cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
        if (stop_) return;
        Job job = std::move(queue_.front());
        queue_.pop_front();
        ++running_;
        progress_[job.id] = 0;
        lock.unlock();
        Result r = run(job);
        lock.lock();
        progress_.erase(job.id);
        --running_;
        ++done_;
        finished_.push_back(std::move(r));
    }
}

Converters::Result Converters::run(const Job& job) {
    Result r;
    r.id = job.id;
    r.converter = job.converter->id;
    r.input = job.input;
    r.out = job.out;
    if (python_.empty()) {
        r.error = "не найден Python: конвертеры работают через него";
        return r;
    }
    std::error_code ec;
    fs::remove_all(job.out, ec);
    fs::create_directories(job.out, ec);
    // The job is a file next to the output: no quoting troubles with names.
    fs::path job_file = job.out;
    job_file += ".job.json";
    const std::string text = "{\"converter\":" + json_text(job.converter->id) + ",\"folder\":" +
                             json_text(path_to_utf8(job.converter->folder)) + ",\"input\":" + json_text(path_to_utf8(job.input)) +
                             ",\"output\":" + json_text(path_to_utf8(job.out)) + ",\"settings\":" + job.settings + "}";
    if (!write_file_atomic(job_file, std::span(reinterpret_cast<const u8*>(text.data()), text.size()))) {
        r.error = "не записывается задание " + path_to_utf8(job_file);
        return r;
    }
    const std::string py = path_to_utf8(python_), runner = path_to_utf8(job.converter->runner),
                      jf = path_to_utf8(job_file);
    const char* args[] = {py.c_str(), "-X", "utf8", runner.c_str(), jf.c_str(), nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) {
        r.error = std::string("Python не запустился: ") + SDL_GetError();
        fs::remove(job_file, ec);
        return r;
    }
    {
        std::lock_guard lock(mutex_);
        processes_[job.id] = process;
    }
    SDL_IOStream* out = static_cast<SDL_IOStream*>(
        SDL_GetPointerProperty(SDL_GetProcessProperties(process), SDL_PROP_PROCESS_STDOUT_POINTER, nullptr));
    std::string pending, last;
    char buf[4096];
    auto line_done = [&](const std::string& line) {
        yyjson_doc* doc = yyjson_read(line.data(), line.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (yyjson_is_obj(root)) {
            if (yyjson_val* p = yyjson_obj_get(root, "progress")) {
                std::lock_guard lock(mutex_);
                progress_[job.id] = static_cast<f32>(yyjson_get_num(p));
                const std::string m = text_of(yyjson_obj_get(root, "message"));
                if (!m.empty()) message_ = path_to_utf8(job.input.filename()) + ": " + m;
            } else if (yyjson_val* ok = yyjson_obj_get(root, "ok")) {
                r.ok = yyjson_get_bool(ok);
                r.error = text_of(yyjson_obj_get(root, "error"));
                if (yyjson_val* trace = yyjson_obj_get(root, "trace")) FORGE_WARN("%s", text_of(trace).c_str());
                usize i, n;
                yyjson_val* f;
                yyjson_arr_foreach(yyjson_obj_get(root, "files"), i, n, f) r.files.push_back(utf8_path(text_of(f)));
            }
        }
        yyjson_doc_free(doc);
    };
    while (out) {
        const usize got = SDL_ReadIO(out, buf, sizeof(buf));
        if (got == 0) {
            if (SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) {
                SDL_Delay(5);
                continue;
            }
            break;
        }
        pending.append(buf, got);
        for (usize nl; (nl = pending.find('\n')) != std::string::npos;) {
            line_done(pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) line_done(pending);
    int code = 0;
    SDL_WaitProcess(process, true, &code);
    {
        std::lock_guard lock(mutex_);
        processes_.erase(job.id);
    }
    SDL_DestroyProcess(process);
    fs::remove(job_file, ec);
    if (!r.ok && r.error.empty())
        r.error = code == 0 ? "конвертер ничего не сказал" : "конвертер прервался (код " + std::to_string(code) + ")";
    if (!r.ok) r.files.clear();
    return r;
}

} // namespace forge::editor_app
