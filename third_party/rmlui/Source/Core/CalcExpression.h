#pragma once

// Forge: CSS math functions calc(), min(), max() and clamp().
//
// Most expressions only mix one kind of unit once variables are substituted, e.g. calc(-0.5 * 1.5rem), and are folded into a plain
// value as text before the regular property parsers see it. Expressions mixing units whose ratio is only known per element, such as
// calc(100% - 2rem) or calc(1.375rem + 1.5vw), are kept as a Unit::CALC property holding the expression text, and resolved when the
// element's values are computed: into pixels, plus a percentage of the containing block for properties that take percentages.

#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

namespace Calc {

	// True for inherit, initial, unset, revert and revert-layer (lowercase).
	bool IsCssWideKeyword(const String& value);

	// True if the value contains a math function call.
	bool ContainsMath(const String& value);

	// Replaces every math function in the value that can be reduced to a single number with a unit by that value. Returns false
	// when a math function is malformed. Sets 'out_unresolved' when a well-formed math function mixes per-element units.
	bool Fold(const String& value, String& out_value, bool& out_unresolved);

	struct Context {
		float font_size = 16.f;
		float root_font_size = 16.f;
		float dp_ratio = 1.f;
		Vector2f viewport = {1.f, 1.f};
		// Used only to decide min() and max() between lengths and percentages.
		float percent_base_guess = -1.f;
	};

	struct Result {
		float px = 0.f;      // length in pixels
		float percent = 0.f; // percentage of the property's reference size
		float number = 0.f;  // unitless part
		bool valid = false;
	};

	// Resolves a single math function, as stored in a Unit::CALC property.
	Result Resolve(const String& expression, const Context& context);

} // namespace Calc

} // namespace Rml
