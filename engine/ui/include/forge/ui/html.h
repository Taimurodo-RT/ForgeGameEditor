#pragma once

// Web pages as UI documents. A .html file loads like an .rml one: the engine
// turns its HTML into the XML RmlUi reads, and a .css file is a style sheet
// like .rcss. Browser defaults (what a <p>, <h1> or <ul> looks like before any
// style) come from ui/web/html.rcss, linked first so page styles win.
//
// What changes on the way:
//   <!DOCTYPE>, <meta>, <script>, <title> and <base> are dropped;
//   <html> becomes <rml>; a page without <head> or <body> gets them;
//   void tags (<br>, <img>, <input>, ...) are closed: <br/>;
//   bare attributes get a value (disabled -> disabled="disabled") and
//   unquoted values get quotes;
//   <link rel="stylesheet" href="a.css"> becomes <link type="text/rcss" href="a.css"/>;
//   <style> keeps its text, and the content of <style> and comments is not touched.

#include "forge/core/types.h"

#include <string>
#include <string_view>

namespace forge::ui {

// ua_sheet: the href of the browser-defaults sheet to link before the page's
// own styles (empty: none).
std::string html_to_rml(std::string_view html, std::string_view ua_sheet = {});

} // namespace forge::ui
