#pragma once

#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/Texture.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
namespace SVG {
	struct SVGData;
}

/*
    Forge: CSS border-image. A picture cut into nine parts by border-image-slice: the corners keep their shape, the edges are
    stretched, repeated, rounded or spaced along the sides (border-image-repeat), the middle is drawn only with 'fill'. The
    parts cover the border area given by border-image-width, pushed out by border-image-outset.
    An SVG picture is rasterized for the element at the scale of its corners, so the slices fall on whole pixels of the
    window and the corners are drawn pixel for pixel.
 */
class ElementBorderImage {
public:
	/// Loads the picture from border-image-source; false when the element has none.
	bool Instance(Element* element);
	/// Builds the geometry for the element's current size.
	void Generate(Element* element);
	void Render(Element* element);
	void Release();
	bool Active() const { return active; }

private:
	bool active = false;
	Texture texture;
	Geometry geometry;
	String svg_path;                  // an SVG picture's path; empty for other pictures
	SharedPtr<SVG::SVGData> svg;      // its texture at the element's scale
};

} // namespace Rml
