#include "forge/ui/tokens.h"

#include <doctest/doctest.h>

#include <filesystem>

using forge::ui::Tokens;

namespace {

const char* kTokens = R"J({
  "color": {
    "themes": [{"id": "dark"}, {"id": "light"}],
    "tokens": [
      {"name": "primary", "value": {"dark": "#000000", "light": "#ffffff"}},
      {"name": "on-primary", "value": {"dark": "#ffffff", "light": "#000000"}}
    ]
  },
  "radius": {"tokens": [{"name": "radius-md", "value": "12px"}]},
  "duration": {"tokens": [{"name": "duration-short", "value": "150ms"}]},
  "type": {
    "families": {"display": "\"Exo 2\", \"Segoe UI\", sans-serif"},
    "groups": [{"family": "display", "styles": [
      {"name": "title-large", "fontSize": "22px", "lineHeight": "28px", "fontWeight": 700}
    ]}]
  }
})J";

const char* kOverrides = R"J({
  "tokens": {
    "button-radius": "{radius-md}",
    "primary-hover": "mix(primary, on-primary, 0.5)",
    "hover-layer": "alpha(primary-hover, 0.5)"
  },
  "themes": {"light": {"radius-md": "4px"}}
})J";

Tokens make() {
    Tokens t;
    std::string error;
    REQUIRE_MESSAGE(t.load_from_text(kTokens, kOverrides, &error), error);
    return t;
}

} // namespace

TEST_CASE("ui tokens: themes and per-theme colours") {
    Tokens t = make();
    CHECK(t.themes().size() == 2);
    CHECK(t.theme() == "dark");
    REQUIRE(t.find("primary"));
    CHECK(*t.find("primary") == "#000000");
    CHECK(t.set_theme("light"));
    CHECK(*t.find("primary") == "#ffffff");
    CHECK_FALSE(t.set_theme("no-such-theme"));
    CHECK(t.theme() == "light");
}

TEST_CASE("ui tokens: references, overrides and computed colours") {
    Tokens t = make();
    CHECK(*t.find("button-radius") == "12px");
    // mix order of evaluation does not matter: hover-layer uses primary-hover.
    CHECK(*t.find("primary-hover") == "#808080ff");
    CHECK(*t.find("hover-layer") == "#80808080");
    t.set_theme("light");
    CHECK(*t.find("button-radius") == "4px");
    CHECK(*t.find("primary-hover") == "#808080ff");
}

TEST_CASE("ui tokens: units and typography for RmlUi") {
    Tokens t = make();
    CHECK(*t.find("duration-short") == "0.15s");
    CHECK(*t.find("font-display") == "Exo 2");
    CHECK(*t.find("title-large-size") == "22px");
    CHECK(*t.find("title-large-weight") == "700");
    CHECK(*t.find("title-large-letter-spacing") == "0px");
}

TEST_CASE("ui tokens: substitute") {
    Tokens t = make();
    std::vector<std::string> missing;
    CHECK(t.substitute("a { border-radius: var(--radius-md); }") == "a { border-radius: 12px; }");
    // Not a token: left for RmlUi (a custom property of the style itself).
    CHECK(t.substitute("color: var(--nope, red);", &missing) == "color: var(--nope, red);");
    REQUIRE(missing.size() == 1);
    CHECK(missing[0] == "nope");
    // Not a var() call: left as is.
    CHECK(t.substitute("somevar(--radius-md)") == "somevar(--radius-md)");
}

TEST_CASE("ui tokens: the shipped design system loads") {
    Tokens t;
    std::string error;
    REQUIRE_MESSAGE(t.load(std::filesystem::path(FORGE_SOURCE_DIR) / "ui/forge-ui/tokens.json", &error), error);
    CHECK(t.themes().size() >= 4);
    for (const std::string theme : t.themes()) {
        t.set_theme(theme);
        for (const char* name : {"surface", "primary", "primary-hover", "state-hover-layer", "button-radius"}) {
            INFO(theme, " ", name);
            const std::string* value = t.find(name);
            REQUIRE(value);
            CHECK(!value->empty());
            CHECK(*value != "#ff00ff");
        }
    }
}
