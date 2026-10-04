#pragma once

// Converters: Python scripts that turn a file into others («Photoshop →
// PNG», «Aseprite → лист кадров», «Звук → OGG»). Each is a script and a
// description next to it (name.py, name.converter.json): what it is called,
// which files it takes, its settings in plain words. Any folder of them
// works: the editor's own converters/ and the project's converters/.
//
// A conversion runs as a separate Python process (a broken converter cannot
// take the editor down), several at once; the results are written into a
// new folder and the editor moves them next to the source file.
//
//   Converters c;
//   c.load({editor_dir, project / "converters"});
//   c.start(*c.find("audio"), "шаг.wav", staging, R"({"format":"ogg"})");
//   ... each frame: for (auto& r : c.take_finished()) ...

#include "forge/core/types.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace forge::editor_app {

struct ConverterChoice {
    std::string id, name;
};

struct ConverterSetting {
    std::string id, name, hint;
    std::string type; // "choice", "bool", "number", "text"
    std::vector<ConverterChoice> choices;
    std::string value; // the default, as JSON ("\"ogg\"", "true", "0.6")
    f64 min = 0, max = 1, step = 0.1;
    // True when the JSON value fits: one of the choices, a number in range...
    bool accepts(const std::string& json) const;
};

struct Converter {
    std::string id; // the script's name: "audio"
    std::string name, about, icon;
    std::vector<std::string> from; // ".wav", ".mp3"
    std::vector<ConverterSetting> settings;
    std::filesystem::path folder;
    std::filesystem::path runner; // forge_convert.py: its folder's, else the editor's

    bool takes(const std::string& extension) const;
};

class Converters {
public:
    Converters();
    ~Converters();
    Converters(const Converters&) = delete;
    Converters& operator=(const Converters&) = delete;

    // Reads every *.converter.json in the folders (missing ones are fine);
    // the first folder has the runner (forge_convert.py) for all of them.
    void load(const std::vector<std::filesystem::path>& folders);
    const std::vector<Converter>& all() const { return list_; }
    const Converter* find(const std::string& id) const;
    // The Python the converters run with: the FORGE_PYTHON environment
    // variable, else the editor's own, else python3 / python on the PATH.
    // Empty: none found.
    const std::filesystem::path& python() const { return python_; }

    struct Result {
        u64 id = 0;
        std::string converter;
        std::filesystem::path input, out;
        bool ok = false;
        std::string error;
        std::vector<std::filesystem::path> files; // what it made, inside out
    };
    // Queues a conversion of input into the folder out (made empty), with
    // settings as a JSON object; returns its id.
    u64 start(const Converter& c, const std::filesystem::path& input, const std::filesystem::path& out,
              const std::string& settings);
    std::vector<Result> take_finished();
    // Of the conversions started since the last idle moment.
    u32 running() const;
    u32 done() const;
    u32 total() const;
    f32 progress() const; // 0..1 over all of them
    std::string message() const;
    // Stops the queued ones and kills the running ones.
    void stop();

private:
    struct Job {
        u64 id = 0;
        const Converter* converter = nullptr;
        std::filesystem::path input, out;
        std::string settings;
    };
    void worker_main();
    Result run(const Job& job);

    std::vector<Converter> list_;
    std::filesystem::path python_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::vector<Result> finished_;
    std::vector<std::thread> workers_;
    std::unordered_map<u64, f32> progress_;      // of the running ones
    std::unordered_map<u64, void*> processes_;   // SDL_Process*, to kill on stop
    std::string message_;
    u64 next_id_ = 1;
    u32 total_ = 0, done_ = 0, running_ = 0;
    bool stop_ = false;
};

} // namespace forge::editor_app
