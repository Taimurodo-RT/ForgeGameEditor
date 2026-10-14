#pragma once

// Forge: the scale an SVG picture is rasterized at under the transforms of its element and the element's ancestors (step 4
// of docs/models/вектор-модель.md). Without a transform it is 1: the raster is the box's size in window pixels, as before.
// A scale that holds is taken as it is. One that keeps changing (an animation) goes by steps of 2^(1/4), and between two
// steps the last raster is stretched; once the scale has held for kStill frames drawn, the picture is made at it.

#include "Header.h"

namespace Rml {

class Element;

class RMLUICORE_API PictureScale {
public:
	static constexpr int kStill = 8;

	/// The element's box on the window, under its transforms and its ancestors': the longer of its two axes; 1 without.
	static float Of(const Element* element);
	/// The step at or above the scale: 2^(n/4), from 1/8 to 8.
	static float Step(float scale);

	/// Once each frame the pictures are drawn, with the element's scale now: true when the scale of their raster changed,
	/// and they are to be made again.
	bool Update(float scale);
	/// The scale to make the pictures at (1 until the first Update).
	float Raster() const { return raster; }

private:
	float seen = 0.f; // 0: none seen yet
	float raster = 1.f;
	int still = 0; // frames the scale has held
};

} // namespace Rml
