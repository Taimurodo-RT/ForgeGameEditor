#pragma once

#include "Types.h"

namespace Rml {

/**
    Metrics and bitmap data for a single glyph within a font face.
 */

class RMLUICORE_API FontGlyph {
public:
	/// The distance from the cursor (positioned vertically on the baseline) to the top-left corner of this glyph's bitmap.
	Vector2i bearing;
	/// The glyph's advance; this is how far the cursor will be moved along after rendering this character.
	int advance = 0;
	/// Forge: the unhinted, fractional advance; text is laid out with these, as browsers and design tools do.
	float advance_exact = 0.f;

	/// Bitmap data defining this glyph. The dimensions and format of the data is given below. This will be nullptr if the glyph has no bitmap data.
	const byte* bitmap_data = nullptr;

	Vector2i bitmap_dimensions;
	ColorFormat color_format = ColorFormat::A8;

	// bitmap_data may point to this member or another font glyph data.
	UniquePtr<byte[]> bitmap_owned_data;

	// Create a copy with its bitmap data owned by another glyph.
	FontGlyph WeakCopy() const
	{
		FontGlyph glyph;
		glyph.bearing = bearing;
		glyph.advance = advance;
		glyph.advance_exact = advance_exact;
		glyph.bitmap_data = bitmap_data;
		glyph.bitmap_dimensions = bitmap_dimensions;
		glyph.color_format = color_format;
		return glyph;
	}
};

using FontGlyphMap = UnorderedMap<Character, FontGlyph>;

} // namespace Rml
