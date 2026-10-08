#include "forge/audio/audio.h"
#include "forge/audio/screen_sounds.h"
#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/ui_design.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace d = forge::editor::design;
namespace audio = forge::audio;
using forge::f32;
using forge::u32;
using forge::u8;

namespace {

std::string read_text(const std::filesystem::path& p) {
    std::vector<u8> bytes;
    if (!forge::read_file(p, bytes)) return {};
    std::string out(bytes.begin(), bytes.end());
    std::erase(out, '\r'); // a Windows checkout may turn line ends into CRLF
    return out;
}

// A clip as a small WAV: mono, 16-bit, 22 050 Hz (the examples' files stay small).
std::vector<u8> small_wav(const audio::Clip& clip) {
    constexpr u32 rate = 22050;
    const u32 frames = static_cast<u32>(static_cast<double>(clip.frames()) * rate / audio::kRate);
    std::vector<u8> out;
    auto u32le = [&](u32 v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
    };
    auto u16le = [&](forge::u16 v) {
        out.push_back(static_cast<u8>(v));
        out.push_back(static_cast<u8>(v >> 8));
    };
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    u32le(36 + frames * 2);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32le(16);
    u16le(1);
    u16le(1);
    u32le(rate);
    u32le(rate * 2);
    u16le(2);
    u16le(16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    u32le(frames * 2);
    for (u32 i = 0; i < frames; ++i) {
        const u32 at = std::min(static_cast<u32>(static_cast<double>(i) * audio::kRate / rate), clip.frames() - 1);
        const f32 v = std::clamp((clip.samples[at * 2] + clip.samples[at * 2 + 1]) * 0.5f, -1.0f, 1.0f);
        u16le(static_cast<forge::u16>(static_cast<forge::i16>(std::lround(v * 32000))));
    }
    return out;
}

// A tune of notes one after another (Hz), each `step` seconds.
audio::ClipPtr tune(std::initializer_list<f32> notes, f32 step, audio::Wave wave) {
    std::vector<audio::Tone> tones;
    f32 at = 0;
    for (f32 hz : notes) {
        tones.push_back({wave, hz, hz, step * 0.95f, 0.01f, 3, 0.35f, at});
        at += step;
    }
    return audio::synth(tones);
}

// The example's sounds: two tunes, a click and a ring.
struct NamedClip {
    const char* name;
    audio::ClipPtr clip;
};
std::vector<NamedClip> example_sounds() {
    const audio::Tone click[] = {{audio::Wave::Square, 1200, 900, 0.05f, 0.001f, 40, 0.3f}};
    const audio::Tone ring[] = {{audio::Wave::Sine, 1320, 1320, 0.35f, 0.002f, 9, 0.4f}, {audio::Wave::Sine, 1980, 1980, 0.3f, 0.002f, 12, 0.2f}};
    return {{"мелодия меню.wav", tune({392, 494, 587, 494, 440, 523, 659, 523}, 0.2f, audio::Wave::Triangle)},
            {"мелодия игры.wav", tune({262, 330, 392, 330, 294, 349, 440, 349}, 0.2f, audio::Wave::Sine)},
            {"мелодия окна.wav", tune({523, 440, 349, 440}, 0.3f, audio::Wave::Triangle)},
            {"щелчок.wav", audio::synth(click)},
            {"звон.wav", audio::synth(ring)}};
}

f32 peak(audio::Mixer& m, u32 frames = 2048) {
    std::vector<f32> buf(static_cast<forge::usize>(frames) * 2);
    m.mix(buf.data(), frames);
    f32 p = 0;
    for (f32 v : buf) p = std::max(p, std::fabs(v));
    return p;
}

std::filesystem::path sound_folder() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / forge::utf8_path("forge_tests_звуки_экрана");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    for (const NamedClip& s : example_sounds()) {
        std::vector<u8> wav;
        REQUIRE(audio::encode_wav(*s.clip, wav));
        REQUIRE(forge::write_file_atomic(dir / forge::utf8_path(s.name), wav));
    }
    return dir;
}

d::Node box(const char* name, f32 x, f32 y, f32 w, f32 h, d::Color fill) {
    d::Node n;
    n.type = d::NodeType::Frame;
    n.name = name;
    n.x = x;
    n.y = y;
    n.w = w;
    n.h = h;
    n.fills.push_back({});
    n.fills[0].color = fill;
    n.radius = {14, 14, 14, 14};
    return n;
}

d::Node text(const char* name, const char* words, f32 x, f32 y, f32 w, f32 h, f32 size, d::Color color) {
    d::Node t;
    t.type = d::NodeType::Text;
    t.name = name;
    t.text = words;
    t.x = x;
    t.y = y;
    t.w = w;
    t.h = h;
    t.text_style.size = size;
    t.text_style.align = d::TextAlign::Center;
    t.text_style.color = color;
    return t;
}

d::Node button(const char* name, const char* label, f32 x, f32 y, f32 w, std::vector<d::Action> actions, const char* sound) {
    d::Node b = box(name, x, y, w, 90, {232, 176, 74, 255});
    b.on_click = std::move(actions);
    b.click_sound = sound;
    b.children.push_back(text("Надпись", label, 0, 0, w, 90, 36, {27, 33, 39, 255}));
    return b;
}

void finish(d::Screen& s) {
    for (d::Node& n : s.root.children) d::renumber(s, n);
}

// games/examples/screen-sound: the main menu with its music and its buttons' click (one rings its own bell, one is
// quiet), a button over the game that opens a window, and the window, with a music of its own.
d::Screen menu_screen() {
    d::Screen s = d::make_screen("Звук: меню", 1920, 1080);
    s.show = d::ScreenShow::Menu;
    s.music = "мелодия меню.wav";
    s.button_sound = "щелчок.wav";
    s.root.fills.push_back({});
    s.root.fills[0].color = {24, 30, 38, 255};
    s.root.children = {text("Заголовок", "Звук экрана", 460, 120, 1000, 90, 64, {255, 255, 255, 255}),
                       button("Нажми", "Нажми", 760, 300, 400, {{d::ActionKind::Change, "demo.presses += 1"}}, ""),
                       button("Звон", "Звон", 760, 420, 400, {{d::ActionKind::Change, "demo.rings += 1"}}, "звон.wav"),
                       button("Тихо", "Тихо", 760, 540, 400, {{d::ActionKind::Change, "demo.quiet += 1"}}, "none"),
                       text("Счёт", "Нажми: {demo.presses}   Звон: {demo.rings}   Тихо: {demo.quiet}", 360, 700, 1200, 60, 32,
                            {200, 210, 220, 255})};
    finish(s);
    return s;
}

d::Screen game_screen() {
    d::Screen s = d::make_screen("Звук: игра", 1920, 1080);
    s.show = d::ScreenShow::Playing;
    s.music = "мелодия игры.wav";
    s.button_sound = "щелчок.wav";
    s.root.children = {button("Окно", "Окно", 40, 40, 240, {{d::ActionKind::Show, "звук_окно"}}, "")};
    finish(s);
    return s;
}

d::Screen window_screen() {
    d::Screen s = d::make_screen("Звук: окно", 1920, 1080);
    s.show = d::ScreenShow::Command;
    s.pauses = true;
    s.music = "мелодия окна.wav";
    s.root.fills.push_back({});
    s.root.fills[0].color = {0, 0, 0, 120};
    d::Node panel = box("Окно", 560, 240, 800, 600, {36, 48, 63, 255});
    panel.children = {text("Заголовок", "Окно со своей музыкой", 0, 40, 800, 70, 44, {255, 255, 255, 255}),
                      button("Звон", "Звон", 200, 220, 400, {{d::ActionKind::Change, "demo.rings += 1"}}, "звон.wav"),
                      button("Закрыть", "Закрыть", 200, 360, 400, {{d::ActionKind::Close, ""}}, "")};
    s.root.children = {panel};
    finish(s);
    return s;
}

const std::filesystem::path kExample = std::filesystem::path(FORGE_SOURCE_DIR) / "games" / "examples" / "screen-sound";

void write_example(const char* name, const d::Screen& s) {
    const std::string json = d::save_screen(s), html = d::screen_html(s);
    const std::filesystem::path json_file = kExample / forge::utf8_path(std::string(name) + ".json");
    const std::filesystem::path html_file = kExample / forge::utf8_path(std::string(name) + ".html");
    if (std::getenv("FORGE_WRITE_EXAMPLES")) {
        std::filesystem::create_directories(kExample);
        REQUIRE(forge::write_file_atomic(json_file, std::span(reinterpret_cast<const u8*>(json.data()), json.size())));
        REQUIRE(forge::write_file_atomic(html_file, std::span(reinterpret_cast<const u8*>(html.data()), html.size())));
    }
    d::Screen back;
    REQUIRE(d::load_screen(read_text(json_file), back));
    CHECK(d::save_screen(back) == json);
    CHECK(read_text(html_file) == html);
}

} // namespace

TEST_CASE("screen sound: one music at a time, started once, stopped when left") {
    const std::filesystem::path dir = sound_folder();
    audio::Mixer mixer; // no device: it plays silently and mix() gives what would be heard
    audio::ScreenSounds s;
    s.attach(&mixer, dir);

    s.music("мелодия меню.wav");
    CHECK(s.music_name() == "мелодия меню.wav");
    CHECK(s.music_playing());
    CHECK(s.music_starts() == 1);
    CHECK(mixer.voices() == 1);
    CHECK(peak(mixer) > 0.05f); // heard: real samples, not only a voice

    // The same music again (a page loaded again, every frame): it goes on, nothing piles up.
    for (int i = 0; i < 10; ++i) s.music("мелодия меню.wav");
    CHECK(s.music_starts() == 1);
    CHECK(mixer.voices() == 1);
    CHECK(mixer.started() == 1);

    // Another screen's music: the first stops, the other starts; one voice.
    const audio::Voice first = s.music_voice();
    s.music("мелодия окна.wav");
    CHECK_FALSE(mixer.playing(first));
    CHECK(s.music_starts() == 2);
    CHECK(mixer.voices() == 1);

    // Back to the first: it starts again (from its beginning).
    s.music("мелодия меню.wav");
    CHECK(s.music_starts() == 3);
    CHECK(mixer.voices() == 1);

    // No screen with music: silence. Leaving («Проверить», the game): the same.
    s.music({});
    CHECK(s.music_name().empty());
    CHECK(mixer.voices() == 0);
    peak(mixer); // the stopped voice fades out within a block
    CHECK(peak(mixer) == 0);
    s.music("мелодия игры.wav");
    CHECK(mixer.voices() == 1);
    s.detach();
    CHECK(mixer.voices() == 0);
    CHECK_FALSE(s.music_playing());
}

TEST_CASE("screen sound: a button's sound on the interface bus at the game's volumes") {
    const std::filesystem::path dir = sound_folder();
    audio::Mixer mixer;
    audio::ScreenSounds s;
    s.attach(&mixer, dir);

    s.click("щелчок.wav");
    CHECK(s.clicks() == 1);
    CHECK(s.last_click() == "щелчок.wav");
    CHECK(mixer.voices() == 1);
    CHECK(peak(mixer, 256) > 0.05f);
    mixer.stop_all();
    peak(mixer);

    // The game's pause stops the world's sounds, not the pause menu's buttons.
    mixer.pause(audio::Bus::Sound, true);
    s.click("звон.wav");
    CHECK(s.last_click() == "звон.wav");
    CHECK(peak(mixer, 256) > 0.05f);
    mixer.pause(audio::Bus::Sound, false);
    mixer.stop_all();
    peak(mixer);

    // The settings: buttons at the sounds' volume, music at the music's, both under the master.
    mixer.set_volume(audio::Bus::Ui, 0);
    s.click("звон.wav");
    CHECK(peak(mixer, 256) == 0);
    mixer.stop_all();
    peak(mixer);
    mixer.set_volume(audio::Bus::Ui, 1);
    mixer.set_volume(audio::Bus::Music, 0);
    s.music("мелодия меню.wav");
    CHECK(s.music_playing()); // it runs (and is in time when turned up)...
    CHECK(peak(mixer) == 0);  // ...without being heard
    mixer.set_volume(audio::Bus::Music, 0.5f);
    const f32 half = peak(mixer);
    mixer.set_volume(audio::Bus::Music, 1);
    peak(mixer); // the gain ramps across one block
    const f32 full = peak(mixer);
    CHECK(half > 0.02f);
    CHECK(full > half * 1.6f);
    mixer.set_master(0);
    peak(mixer);
    CHECK(peak(mixer) == 0);
}

TEST_CASE("screen sound: a missing or wrong file is said once and never read again") {
    const std::filesystem::path dir = sound_folder();
    audio::Mixer mixer;
    audio::ScreenSounds s;
    s.attach(&mixer, dir);
    for (int i = 0; i < 5; ++i) {
        s.click("нет такого.wav");
        s.music("нет такой.ogg");
    }
    CHECK(s.clicks() == 0);
    CHECK(s.music_name() == "нет такой.ogg");
    CHECK_FALSE(s.music_playing());
    CHECK(s.music_starts() == 0);
    CHECK(mixer.voices() == 0);
    CHECK(s.problems() == std::vector<std::string>{"нет такого.wav", "нет такой.ogg"});

    // Not a file of the sounds folder, not a sound the game reads: silent, said once.
    s.click("../звон.wav");
    s.click("../звон.wav");
    s.click("музыка.mp3");
    CHECK(s.clicks() == 0);
    CHECK(s.problems().size() == 4);

    // A broken file: said once.
    const char junk[] = "not a sound";
    REQUIRE(forge::write_file_atomic(dir / forge::utf8_path("сломан.wav"), std::span(reinterpret_cast<const u8*>(junk), sizeof junk)));
    s.click("сломан.wav");
    s.click("сломан.wav");
    CHECK(s.problems().size() == 5);

    // Added later: read when asked again after forget() (the editor's «Проверить» does it on entry).
    std::vector<u8> wav;
    REQUIRE(audio::encode_wav(*example_sounds()[3].clip, wav));
    REQUIRE(forge::write_file_atomic(dir / forge::utf8_path("нет такого.wav"), wav));
    s.click("нет такого.wav");
    CHECK(s.clicks() == 0);
    s.forget();
    s.click("нет такого.wav");
    CHECK(s.clicks() == 1);
}

TEST_CASE("screen sound: the screen's music and buttons' sounds survive saving, old screens open as they were") {
    d::Screen s = menu_screen();
    const std::string json = d::save_screen(s);
    d::Screen back;
    REQUIRE(d::load_screen(json, back));
    CHECK(back.music == "мелодия меню.wav");
    CHECK(back.button_sound == "щелчок.wav");
    CHECK(back.root.children[1].click_sound.empty());
    CHECK(back.root.children[2].click_sound == "звон.wav");
    CHECK(back.root.children[3].click_sound == "none");
    CHECK(d::save_screen(back) == json);

    // A screen saved before 13.7: no sound, nothing else changes.
    d::Screen old = s;
    old.music.clear();
    old.button_sound.clear();
    for (d::Node& n : old.root.children) n.click_sound.clear();
    const std::string old_json = d::save_screen(old);
    CHECK(old_json.find("music") == std::string::npos);
    CHECK(old_json.find("sound") == std::string::npos);
    d::Screen reopened;
    REQUIRE(d::load_screen(old_json, reopened));
    CHECK(reopened.music.empty());
    CHECK(d::save_screen(reopened) == old_json);
    CHECK(d::screen_html(reopened).find("forge-music") == std::string::npos);
}

TEST_CASE("screen sound: the page says what plays") {
    const std::string html = d::screen_html(menu_screen());
    CHECK(html.find("forge-screen=\"menu\"") != std::string::npos);
    CHECK(html.find("forge-music=\"мелодия меню.wav\" forge-button-sound=\"щелчок.wav\"") != std::string::npos);
    // «Как у экрана»: no attribute; its own; «Без звука».
    const d::Screen s = menu_screen();
    auto tag = [&](u32 id) {
        const std::string head = "<div id=\"n" + std::to_string(id) + "\"";
        const auto at = html.find(head);
        REQUIRE(at != std::string::npos);
        return html.substr(at, html.find('>', at) - at);
    };
    CHECK(tag(s.root.children[1].id).find("forge-click-sound") == std::string::npos);
    CHECK(tag(s.root.children[2].id).find("forge-click-sound=\"звон.wav\"") != std::string::npos);
    CHECK(tag(s.root.children[3].id).find("forge-click-sound=\"none\"") != std::string::npos);
    // A sound on a layer that does nothing when clicked is not written (it has no press).
    d::Screen quiet = s;
    quiet.root.children[2].on_click.clear();
    CHECK(d::screen_html(quiet).find("звон.wav") == std::string::npos);
    // Names that need escaping stay one attribute.
    d::Screen odd = s;
    odd.music = "\"a&b\".wav";
    CHECK(d::screen_html(odd).find("forge-music=\"&quot;a&amp;b&quot;.wav\"") != std::string::npos);

    // The keyboard (Tab, Enter, Space) presses buttons on the menu and on windows that stop the game; over a
    // running game Space and Enter stay the game's.
    CHECK(html.find("tab-index: auto") != std::string::npos);
    CHECK(d::screen_html(game_screen()).find("tab-index") == std::string::npos);
    CHECK(d::screen_html(window_screen()).find("tab-index: auto") != std::string::npos);
    d::Screen open_window = window_screen();
    open_window.pauses = false;
    CHECK(d::screen_html(open_window).find("tab-index") == std::string::npos);
}

TEST_CASE("screen sound: a frame that clips hides its layers from the pointer too") {
    // A clipped part of a button must not sound or press: RmlUi clips freely placed layers only with «clip:
    // always». The screen itself is the window: it needs none.
    d::Screen s = menu_screen();
    s.root.clip = true;
    d::Node frame = box("Рамка", 100, 100, 300, 200, {40, 50, 60, 255});
    frame.clip = true;
    d::renumber(s, frame);
    s.root.children.push_back(frame);
    d::Node open = box("Без обрезки", 500, 100, 300, 200, {40, 50, 60, 255});
    d::renumber(s, open);
    s.root.children.push_back(open);
    const std::string html = d::screen_html(s);
    auto rule = [&](u32 id) {
        const std::string head = "#n" + std::to_string(id) + " {\n";
        const auto at = html.find(head);
        REQUIRE(at != std::string::npos);
        return html.substr(at, html.find('}', at) - at);
    };
    CHECK(rule(s.root.children[s.root.children.size() - 2].id).find("clip: always") != std::string::npos);
    CHECK(rule(s.root.children.back().id).find("clip:") == std::string::npos);
    CHECK(rule(s.root.id).find("clip: always") == std::string::npos);
}

TEST_CASE("screen sound: a button in its «Выключена» look is off in the game") {
    d::Screen lib = d::make_screen("Компоненты", 1920, 1080);
    lib.library = true;
    for (const char* state : {"Обычная", "Выключена"}) {
        d::Node b = box("Кнопка", 0, 0, 300, 90, {232, 176, 74, 255});
        b.component = "Кнопка";
        b.variant = {{"Состояние", state}};
        d::renumber(lib, b);
        lib.root.children.push_back(b);
    }
    d::Screen s = menu_screen();
    for (const char* state : {"Обычная", "Выключена"}) {
        std::optional<d::Node> inst = d::make_instance(s, lib, "Кнопка");
        REQUIRE(inst);
        d::set_variant_value(inst->variant, "Состояние", state);
        d::sync_instance(s, *inst, lib);
        inst->on_click = {{d::ActionKind::Change, "demo.presses += 1"}};
        inst->overrides.push_back(d::override_of("click.0.kind"));
        s.root.children.push_back(*inst);
    }
    const u32 on = s.root.children[s.root.children.size() - 2].id, off = s.root.children.back().id;
    d::HtmlOptions options;
    options.library = &lib;
    const std::string html = d::screen_html(s, options);
    auto tag = [&](u32 id) {
        const std::string head = "<div id=\"n" + std::to_string(id) + "\"";
        const auto at = html.find(head);
        REQUIRE(at != std::string::npos);
        return html.substr(at, html.find('>', at) - at);
    };
    CHECK(tag(on).find("forge-disabled") == std::string::npos);
    CHECK(tag(off).find("forge-disabled=\"1\"") != std::string::npos);

    // A copy's own sound is its change: kept when the component changes.
    d::Node& copy = s.root.children[s.root.children.size() - 2];
    copy.click_sound = "звон.wav";
    lib.root.children[0].fills[0].color = {10, 20, 30, 255};
    CHECK(d::sync_instances(s, lib));
    CHECK(s.root.children[s.root.children.size() - 2].click_sound == "звон.wav");
    CHECK(s.root.children[s.root.children.size() - 2].fills[0].color == d::Color{10, 20, 30, 255});
}

TEST_CASE("screen sound: the example screens are as the editor writes them") {
    write_example("звук_меню", menu_screen());
    write_example("звук_игра", game_screen());
    write_example("звук_окно", window_screen());
    // Its sounds: small WAVs the game reads, Russian names.
    for (const NamedClip& c : example_sounds()) {
        const std::filesystem::path file = kExample / "sounds" / forge::utf8_path(c.name);
        if (std::getenv("FORGE_WRITE_EXAMPLES")) {
            std::filesystem::create_directories(file.parent_path());
            REQUIRE(forge::write_file_atomic(file, small_wav(*c.clip)));
        }
        std::string error;
        const audio::ClipPtr clip = audio::load(file, &error);
        CHECK_MESSAGE(clip, c.name, ": ", error);
        if (clip) CHECK(clip->seconds() == doctest::Approx(c.clip->seconds()).epsilon(0.01));
    }
}
