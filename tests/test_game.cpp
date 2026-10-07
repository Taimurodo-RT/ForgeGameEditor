#include "forge/game/dialogue.h"
#include "forge/game/dialogue_source.h"
#include "forge/game/quests.h"
#include "forge/game/saves.h"
#include "forge/game/vars.h"

#include "forge/core/file.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

namespace gametest {
using namespace forge;
using namespace forge::game;

Value eval(const char* src, const Vars& vars, const CallFn& call = {}) {
    std::string error;
    Expr e = Expr::parse(src, &error);
    REQUIRE_MESSAGE(error.empty(), error);
    return e.eval(vars, call);
}

TEST_CASE("game: expressions read variables, compare and combine") {
    Vars v;
    v.set("quest.pickaxe", 2);
    v.set("hero.name", "Борин");
    CHECK(eval("quest.pickaxe == 2", v).truthy());
    CHECK(eval("quest.pickaxe >= 1 and not met", v).truthy()); // missing reads as 0
    CHECK_FALSE(eval("quest.pickaxe > 2 or met", v).truthy());
    CHECK(eval("quest.pickaxe * 10 + 1", v).number() == 21);
    CHECK(eval("hero.name == \"Борин\"", v).truthy());
    CHECK(eval("(1 + 2) * 3", v).number() == 9);
    CHECK(eval("-2 + 5", v).number() == 3);
    CHECK(eval("кирка >= 0 и не нет", v).truthy()); // Russian words work too
    CHECK(eval("a != 1 && !b", v).truthy());
    CHECK(eval("1 / 0", v).number() == 0);
    CHECK(eval("\"Привет, \" + hero.name", v).text() == "Привет, Борин");
    CHECK(Expr::parse("", nullptr).test(v)); // empty means true

    // Calls go to the game.
    auto call = [](std::string_view name, const std::vector<Value>& args) -> Value {
        if (name == "has") return Value(args.size() == 1 && args[0].text() == "pickaxe");
        return Value();
    };
    CHECK(eval("has(\"pickaxe\")", v, call).truthy());
    CHECK_FALSE(eval("has(\"sword\")", v, call).truthy());
}

TEST_CASE("game: expression errors are explained") {
    std::string error;
    CHECK(Expr::parse("a ==", &error).empty());
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK(Expr::parse("(a + 1", &error).empty());
    CHECK(error.find(")") != std::string::npos);
    error.clear();
    CHECK(Expr::parse("a b", &error).empty());
    CHECK_FALSE(error.empty());
    error.clear();
    Expr::parse_actions("gold 5", &error);
    CHECK_FALSE(error.empty());
}

TEST_CASE("game: actions change variables and call the game") {
    Vars v;
    std::string error;
    std::vector<std::string> called;
    auto call = [&](std::string_view name, const std::vector<Value>& args) -> Value {
        called.push_back(std::string(name) + ":" + (args.empty() ? "" : args[0].text()));
        return Value();
    };
    Expr a = Expr::parse_actions("gold = 10; gold += 5; gold -= 3; greeting = \"привет\"; give(\"pickaxe\")", &error);
    REQUIRE(error.empty());
    a.run(v, call);
    CHECK(v.get("gold").number() == 12);
    CHECK(v.get("greeting").text() == "привет");
    REQUIRE(called.size() == 1);
    CHECK(called[0] == "give:pickaxe");

    std::vector<std::string> calls, names;
    a.collect(&calls, &names);
    CHECK(calls == std::vector<std::string>{"give"});
    CHECK(names.size() == 4);
}

TEST_CASE("game: variables survive JSON and fill in text") {
    Vars v;
    v.set("gold", 12.5);
    v.set("hero.name", "Борин");
    v.set("met", true);
    Vars back;
    REQUIRE(back.from_json(v.to_json()));
    CHECK(back.get("gold").number() == 12.5);
    CHECK(back.get("hero.name").text() == "Борин");
    CHECK(back.get("met").truthy());
    CHECK(substitute("Привет, {hero.name}! У тебя {gold} монет. {{скобка}", back) == "Привет, Борин! У тебя 12.5 монет. {скобка}");
    CHECK(substitute("Привет, {player}!", back) == "Привет, {player}!"); // not set yet: shown as written
    CHECK(Value(3.0).text() == "3");
    CHECK(Value(3) == Value("3"));
}

const char* kMiner = R"J({
  "id": "miner",
  "speakers": {"miner": {"name": "Старый шахтёр", "color": "#e8b04a"}, "hero": "{hero.name}"},
  "start": [{"if": "quest.pickaxe >= 3", "goto": "thanks"}, {"if": "quest.pickaxe == 2", "goto": "found"}, {"goto": "hello"}],
  "nodes": [
    {"id": "hello", "speaker": "miner", "text": "Здравствуй, {hero.name}!", "do": "met_miner = 1", "next": "ask"},
    {"id": "ask", "speaker": "miner", "text": "Поможешь старику?",
     "choices": [
       {"text": "Помогу.", "if": "quest.pickaxe == 0", "do": ["quest.pickaxe = 1"], "goto": "where"},
       {"text": "Расскажи ещё раз.", "if": "quest.pickaxe == 1", "goto": "where"},
       {"text": "Есть вопрос.", "goto": "talk"},
       {"text": "Некогда.", "goto": "end"}]},
    {"id": "where", "speaker": "miner", "text": "Кирка осталась в старой шахте, к западу."},
    {"id": "talk", "speaker": "miner", "text": "Спрашивай.",
     "keywords": [{"words": ["кирк*", "инструмент"], "goto": "where"},
                  {"words": ["старая шахта"], "goto": "mine"}],
     "fallback": "dunno",
     "choices": [{"text": "Пока.", "goto": "end"}]},
    {"id": "mine", "speaker": "miner", "text": "Там темно и вода.", "next": "talk"},
    {"id": "dunno", "speaker": "miner", "text": "Не знаю, о чём ты.", "next": "talk"},
    {"id": "found", "branches": [{"if": "has(\"pickaxe\")", "goto": "give_back"}], "next": "not_yet"},
    {"id": "give_back", "speaker": "miner", "text": "Моя кирка!", "do": "take(\"pickaxe\"); quest.pickaxe = 3; gold += 50"},
    {"id": "not_yet", "speaker": "miner", "text": "Ну что, нашёл?"},
    {"id": "thanks", "speaker": "miner", "text": "Спасибо тебе, {hero.name}."}
  ]
})J";

TEST_CASE("game: a dialogue plays from start to end with choices") {
    Dialogue d;
    DialogueReport report;
    const bool loaded = d.load(kMiner, report, {"has", "take"});
    for (const std::string& e : report.errors) MESSAGE(e);
    REQUIRE(loaded);
    CHECK(report.warnings.empty());

    Vars v;
    v.set("hero.name", "Борин");
    DialogueRunner run(v);
    REQUIRE(run.start(d));
    CHECK(run.node_id() == "hello");
    CHECK(run.line().speaker == "Старый шахтёр");
    CHECK(run.line().color == "#e8b04a");
    CHECK(run.line().text == "Здравствуй, Борин!");
    CHECK(v.get("met_miner").truthy()); // the node's action ran
    CHECK(run.line().choices.empty());
    run.advance();
    REQUIRE(run.node_id() == "ask");
    // "Расскажи ещё раз" is hidden until the quest is taken.
    REQUIRE(run.line().choices.size() == 3);
    CHECK(run.line().choices[0] == "Помогу.");
    REQUIRE(run.choose(0));
    CHECK(v.get("quest.pickaxe").number() == 1);
    CHECK(run.node_id() == "where");
    run.advance(); // no next: the dialogue ends
    CHECK_FALSE(run.active());
    CHECK(run.history().size() == 3);

    // The second time the quest is taken: the other choice shows.
    REQUIRE(run.start(d));
    run.advance();
    REQUIRE(run.line().choices.size() == 3);
    CHECK(run.line().choices[0] == "Расскажи ещё раз.");
    CHECK(run.choose(2)); // Некогда
    CHECK_FALSE(run.active());
}

TEST_CASE("game: typed questions match keywords, junctions branch on the game") {
    Dialogue d;
    DialogueReport report;
    REQUIRE(d.load(kMiner, report));
    Vars v;
    bool has_pickaxe = false;
    std::vector<std::string> taken;
    DialogueRunner run(v, [&](std::string_view name, const std::vector<Value>& args) -> Value {
        if (name == "has") return Value(has_pickaxe);
        if (name == "take") taken.push_back(args[0].text());
        return Value();
    });
    REQUIRE(run.start_at(d, "talk"));
    CHECK(run.line().asks_keyword);
    CHECK(run.ask("А где твоя КИРКА?"));          // prefix match, any case
    CHECK(run.node_id() == "where");
    REQUIRE(run.start_at(d, "talk"));
    CHECK(run.ask("Что за старая шахта?"));        // two words in order
    CHECK(run.node_id() == "mine");
    run.advance(); // back to "talk"
    CHECK_FALSE(run.ask("Как погода?"));            // nothing matched: fallback
    CHECK(run.node_id() == "dunno");
    CHECK(run.history().size() == 4);

    v.set("quest.pickaxe", 2);
    REQUIRE(run.start(d));
    CHECK(run.node_id() == "not_yet"); // junction: no pickaxe yet
    has_pickaxe = true;
    REQUIRE(run.start(d));
    CHECK(run.node_id() == "give_back");
    CHECK(taken == std::vector<std::string>{"pickaxe"});
    CHECK(v.get("quest.pickaxe").number() == 3);
    CHECK(v.get("gold").number() == 50);
    run.advance();
    REQUIRE(run.start(d));
    CHECK(run.line().text == "Спасибо тебе, {hero.name}."); // hero.name was never set: shown as written
}

TEST_CASE("game: broken dialogues are reported") {
    Dialogue d;
    DialogueReport report;
    CHECK_FALSE(d.load(R"J({"nodes": [
        {"id": "a", "text": "x", "next": "nowhere"},
        {"id": "a", "text": "twice"},
        {"id": "b", "text": "y", "choices": [{"text": "c", "if": "x ==", "goto": "end"}]},
        {"id": "lost", "text": "nobody comes here", "do": "boom()"}]})J",
                       report, {"has"}));
    auto has = [](const std::vector<std::string>& list, const char* part) {
        for (const std::string& s : list)
            if (s.find(part) != std::string::npos) return true;
        return false;
    };
    CHECK(has(report.errors, "nowhere"));
    CHECK(has(report.errors, "дважды"));
    CHECK(has(report.errors, "x =="));
    CHECK(has(report.warnings, "«lost» недостижим"));
    CHECK(has(report.warnings, "boom"));

    DialogueReport bad;
    CHECK_FALSE(d.load("not json", bad));
}

TEST_CASE("game: a loop of junctions stops instead of hanging") {
    Dialogue d;
    DialogueReport report;
    REQUIRE(d.load(R"J({"nodes": [{"id": "a", "next": "b"}, {"id": "b", "next": "a"}]})J", report));
    Vars v;
    DialogueRunner run(v);
    CHECK_FALSE(run.start(d));
}

TEST_CASE("game: words are lower-cased and split for keywords") {
    CHECK(to_lower_utf8("КиРкА Ёж ABC") == "кирка еж abc");
    const std::vector<std::string> words = split_words("Где, чёрт возьми, КИРКА?!");
    REQUIRE(words.size() == 4);
    CHECK(words[1] == "черт");
    CHECK(keyword_matches(words, "кирк*"));
    CHECK_FALSE(keyword_matches(words, "кир")); // short words match only as they are
    CHECK(keyword_matches(words, "черт возьми"));
    CHECK_FALSE(keyword_matches(words, "возьми черт"));
}

TEST_CASE("game: a keyword matches the forms of its word") {
    auto hit = [](const char* typed, const char* keyword) { return keyword_matches(split_words(typed), keyword); };
    CHECK(hit("Где шахту найти?", "шахта"));
    CHECK(hit("что в шахте", "шахта"));
    CHECK(hit("за шахтой", "шахта"));
    CHECK_FALSE(hit("ты шахтёр?", "шахта"));
    CHECK(hit("кто такой кузнеца", "кузнец"));
    CHECK(hit("с кузнецами", "кузнец"));
    CHECK(hit("медью", "медь"));
    CHECK_FALSE(hit("медведь", "медь"));
    CHECK(hit("про воду", "вода"));
    CHECK_FALSE(hit("водопад", "вода"));
    CHECK(hit("старую шахту", "старая шахта"));
}

TEST_CASE("game: a dialogue's source is written back as it was read") {
    const char* text = R"J({
      "id": "miner",
      "speakers": {"miner": {"name": "Борис", "color": "#e8b04a"}},
      "start": [{"if": "quest.pickaxe >= 1", "goto": "waiting"}, {"goto": "hello"}],
      "nodes": [
        {"id": "hello", "scene": "Первая встреча", "speaker": "miner", "note": "Сидит на бревне.",
         "text": "Эх, \"путник\"...", "do": ["met = 1", "gold += 2"], "next": "ask"},
        {"id": "ask", "speaker": "miner", "text": "Поможешь?",
         "keywords": [{"words": ["шахта", "кирк*"], "goto": "hello"}], "fallback": "hello",
         "choices": [{"text": "Да.", "do": "quest.pickaxe = 1", "goto": "end"}, {"text": "Нет.", "if": "met == 1"}]},
        {"id": "waiting", "branches": [{"if": "has(\"pickaxe\")", "goto": "hello"}], "next": "ask"}
      ]})J";
    DialogueSource a;
    std::string error;
    REQUIRE(a.parse(text, &error));
    CHECK(a.node("hello")->scene == "Первая встреча");
    CHECK(a.node("hello")->note == "Сидит на бревне.");
    CHECK(a.node("hello")->act == "met = 1; gold += 2");
    CHECK(a.node("waiting")->junction());
    CHECK(a.node("ask")->choices[1].go.empty());
    const std::string out = a.json();
    DialogueSource b;
    REQUIRE(b.parse(out, &error));
    CHECK(b.json() == out);
    CHECK(b.node("hello")->text == "Эх, \"путник\"...");
    CHECK(b.node("ask")->keywords[0].words.size() == 2);
    CHECK(b.node("ask")->choices[1].go == "end");
    CHECK(b.start.size() == 2);
    // The game plays what the editor writes.
    Dialogue d;
    DialogueReport report;
    REQUIRE(d.load(out, report));
    CHECK(report.ok());
    CHECK(d.nodes().size() == 3);
    CHECK(a.fresh_id("hello") == "hello2");
    CHECK(a.index_of("waiting") == 2);
}

TEST_CASE("game: a story calls its parts and jumps between talks") {
    const char* script = R"J({"id": "script", "speakers": {"s": {"name": "Сайори"}},
      "nodes": [
        {"id": "start", "call": "ch1:ch1_main", "next": "after"},
        {"id": "after", "call": "poem", "next": "last"},
        {"id": "last", "speaker": "s", "text": "Конец."},
        {"id": "poem", "speaker": "s", "text": "Стихи.", "next": "deeper"},
        {"id": "deeper", "call": "ch1:l1", "next": "return"}
      ]})J";
    const char* ch1 = R"J({"id": "ch1", "speakers": {},
      "nodes": [
        {"id": "ch1_main", "stage": ["scene bg club_day", "play music t2"], "next": "l1"},
        {"id": "l1", "pose": "1a", "text": "Клуб.", "next": "return"}
      ]})J";
    Dialogue a, b;
    DialogueReport ra, rb;
    REQUIRE(a.load(script, ra));
    REQUIRE(b.load(ch1, rb));
    Vars v;
    DialogueRunner run(v);
    run.set_resolver([&](std::string_view talk) -> const Dialogue* { return talk == "ch1" ? &b : talk == "script" ? &a : nullptr; });
    REQUIRE(run.start(a));
    CHECK(run.node_id() == "l1"); // the call went into the other talk, through the staging
    CHECK(run.dialogue() == &b);
    run.advance(); // return: back to "after", which calls "poem"
    CHECK(run.node_id() == "poem");
    CHECK(run.dialogue() == &a);
    run.advance(); // poem calls l1 as its last step: both returns come back at once
    CHECK(run.node_id() == "l1");
    run.advance();
    CHECK(run.node_id() == "last");
    run.advance();
    CHECK_FALSE(run.active());
    // A return with nothing to come back to ends the talk.
    REQUIRE(run.start_at(b, "l1"));
    run.advance();
    CHECK_FALSE(run.active());
    // A jump into a talk the runner cannot find ends it too.
    DialogueRunner lost(v);
    CHECK_FALSE(lost.start(a));

    DialogueSource src;
    std::string error;
    REQUIRE(src.parse(ch1, &error));
    CHECK(src.node("ch1_main")->stage.size() == 2);
    CHECK(src.node("ch1_main")->silent());
    CHECK(src.node("l1")->pose == "1a");
    DialogueSource again;
    REQUIRE(again.parse(src.json(), &error));
    CHECK(again.json() == src.json());
    CHECK(again.node("ch1_main")->stage[1] == "play music t2");
}

TEST_CASE("game: the quest journal follows quest variables") {
    QuestBook book;
    std::vector<std::string> errors;
    REQUIRE(book.load(R"J([
      {"id": "pickaxe", "title": "Потерянная кирка", "var": "quest.pickaxe",
       "stages": [{"at": 2, "text": "Вернуть кирку шахтёру."}, {"at": 1, "text": "Найти кирку в шахте, {hero.name}."}],
       "done_at": 3, "done_text": "Шахтёр доволен."},
      {"id": "well", "title": "Колодец", "stages": [{"at": 1, "text": "Почистить колодец."}]}
    ])J", errors));
    Vars v;
    v.set("hero.name", "Борин");
    CHECK(book.journal(v).empty());
    v.set("quest.pickaxe", 1);
    auto j = book.journal(v);
    REQUIRE(j.size() == 1);
    CHECK(j[0].state == QuestState::Active);
    CHECK(j[0].text == "Найти кирку в шахте, Борин.");
    v.set("quest.pickaxe", 2);
    v.set("quest.well", 1); // default variable name: quest.<id>
    j = book.journal(v);
    REQUIRE(j.size() == 2);
    CHECK(j[0].text == "Вернуть кирку шахтёру.");
    CHECK(j[1].quest->id == "well");
    v.set("quest.pickaxe", 3);
    j = book.journal(v);
    REQUIRE(j.size() == 2);
    CHECK(j[0].quest->id == "well"); // active ones first
    CHECK(j[1].state == QuestState::Done);
    CHECK(j[1].text == "Шахтёр доволен.");

    QuestBook bad;
    errors.clear();
    CHECK_FALSE(bad.load(R"J([{"id": "x"}, {"title": "no id"}])J", errors));
    CHECK(errors.size() == 2);
}

TEST_CASE("game: save slots copy the session and list newest first") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "forge_test_saves";
    fs::remove_all(dir);
    SaveSlots slots(dir);
    CHECK(slots.list().empty());
    CHECK_FALSE(slots.latest());

    // A new game writes into the session folder.
    REQUIRE(slots.begin_session());
    const std::string text = "progress 1";
    REQUIRE(write_file_atomic(slots.session() / "game.json", {reinterpret_cast<const u8*>(text.data()), text.size()}));
    REQUIRE(write_file_atomic(slots.session() / "world" / "r.0.0.fwr", {reinterpret_cast<const u8*>(text.data()), 3}));

    SlotInfo info;
    info.id = slots.new_id();
    CHECK(info.id == "slot-1");
    info.title = "Сохранение 1";
    info.location = "Деревня";
    info.playtime_s = 125;
    REQUIRE(slots.commit(info));
    CHECK(fs::exists(slots.folder("slot-1") / "world" / "r.0.0.fwr"));
    CHECK(slots.new_id() == "slot-2");

    // Playing on: the session changes, the slot keeps what was saved.
    const std::string later = "progress 2";
    REQUIRE(write_file_atomic(slots.session() / "game.json", {reinterpret_cast<const u8*>(later.data()), later.size()}));
    std::vector<u8> bytes;
    REQUIRE(read_file(slots.folder("slot-1") / "game.json", bytes));
    CHECK(std::string(bytes.begin(), bytes.end()) == "progress 1");

    SlotInfo second;
    second.id = "autosave";
    second.autosave = true;
    REQUIRE(slots.commit(second));
    auto list = slots.list();
    REQUIRE(list.size() == 2);
    for (const SlotInfo& s : list) CHECK(s.saved_at > 0);
    CHECK(list[0].saved_at >= list[1].saved_at);
    const SlotInfo* one = list[0].id == "slot-1" ? &list[0] : &list[1];
    CHECK(one->title == "Сохранение 1");
    CHECK(one->location == "Деревня");
    CHECK(one->playtime_s == 125);

    // Saving over a slot replaces it whole.
    REQUIRE(slots.commit(info));
    REQUIRE(read_file(slots.folder("slot-1") / "game.json", bytes));
    CHECK(std::string(bytes.begin(), bytes.end()) == "progress 2");

    // Loading fills a fresh session from the slot, without slot.json.
    REQUIRE(slots.begin_session("autosave"));
    REQUIRE(read_file(slots.session() / "game.json", bytes));
    CHECK(std::string(bytes.begin(), bytes.end()) == "progress 2");
    CHECK_FALSE(fs::exists(slots.session() / "slot.json"));
    std::string error;
    CHECK_FALSE(slots.begin_session("missing", &error));
    CHECK_FALSE(error.empty());

    CHECK(slots.remove("slot-1"));
    CHECK(slots.list().size() == 1);
    fs::remove_all(dir);
}

TEST_CASE("game: settings load with defaults and clamp what is off") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "forge_test_settings";
    fs::remove_all(dir);
    Settings s = load_settings(dir); // no file yet
    CHECK(s.vsync);
    s.fullscreen = true;
    s.ui_scale = 9;
    s.theme = "parchment";
    REQUIRE(save_settings(dir, s));
    const Settings back = load_settings(dir);
    CHECK(back.fullscreen);
    CHECK(back.theme == "parchment");
    CHECK(back.ui_scale == 3.0f);
    fs::remove_all(dir);

    CHECK(format_playtime(125) == "2 мин");
    CHECK(format_playtime(3900) == "1 ч 05 мин");
}

TEST_CASE("vars: an element of a list reads through to the game's") {
    Vars game;
    game.set("inv.coins", 12);
    Vars row;
    row.set_parent(&game);
    row.set("item.name", "Ключ");
    CHECK(row.get("item.name").text() == "Ключ");
    CHECK(row.get("inv.coins").number() == 12);
    CHECK(row.has("inv.coins"));
    CHECK_FALSE(row.has("inv.key"));
    CHECK(substitute("{item.name}: {inv.coins}", row) == "Ключ: 12");
    // Its own value wins; saving sees only its own.
    row.set("inv.coins", 1);
    CHECK(row.get("inv.coins").number() == 1);
    CHECK(row.to_json().find("item.name") != std::string::npos);
    CHECK(game.get("inv.coins").number() == 12);
}

} // namespace gametest
