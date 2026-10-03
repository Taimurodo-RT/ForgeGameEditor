#pragma once

// Graph -> Luau module.
//
// Each event node becomes a handler of the module (several events of the same
// kind run side by side, each in its own thread). A chain of blocks becomes a
// sequence of statements, slots become nested blocks, and when several wires
// lead into one node the shared part becomes a local function. Pure nodes are
// computed where a block needs them, every time it runs (so "position" is
// fresh after a "move"). Each line of the result knows its node (SourceMap):
// errors, breakpoints and the profiler point at nodes, not lines.

#include "forge/script/graph.h"
#include "forge/script/host.h"

#include <string>
#include <vector>

namespace forge::script {

struct Diagnostic {
    bool error = true; // false: a warning, the graph still compiles
    u32 node = 0;      // 0: the graph as a whole
    std::string pin;
    std::string message; // Russian, for the editor
};

struct CompileOptions {
    // Times every node (calls and time, see ScriptHost::node_profile). Costs
    // a little per node; for the editor's play mode, not for release builds.
    bool profile = false;
};

struct CompileResult {
    bool ok = false;
    std::string source;
    SourceMap map;
    std::vector<Diagnostic> diagnostics;
};

CompileResult compile(const Graph& graph, const NodeLibrary& library, const CompileOptions& options = {});

// Turns a typed value into Luau source for a pin of this type; false when the
// text does not fit the type ("abc" for a number).
bool value_to_lua(ValueType type, std::string_view text, std::string& out);

} // namespace forge::script
