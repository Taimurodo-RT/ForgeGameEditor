#pragma once

#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/Texture.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;

/*
    Forge: CSS border-image. A picture cut into nine parts by border-image-slice: the corners keep their shape, the edges are
    stretched, repeated, rounded or spaced along the sides (border-image-repeat), the middle is drawn only with 'fill'. The
    parts cover the border area given by border-image-width, pushed out by border-image-outset.
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
};

} // namespace Rml
