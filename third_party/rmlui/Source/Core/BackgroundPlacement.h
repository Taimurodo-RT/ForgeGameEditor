#pragma once

#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
struct Mesh;

/*
    Forge: CSS placement of a background layer (an url() image or a gradient) in a web page, from background-size,
    background-position and background-repeat.
*/
struct BackgroundPlacement {
	Vector2f image_size;      // Size of one image (tile), in px.
	Vector2f position;        // Offset of the reference tile from the area's top-left corner.
	bool repeat_x = true, repeat_y = true;
};

/// Computes the placement of an image of the given natural size over an area. Images without an intrinsic size (gradients,
/// SVG pictures) pass 'no_intrinsic_size'; for them 'auto' fills the area (gradients, natural <= 0) or acts as 'contain' (SVG).
/// Returns false when nothing should be drawn.
bool ComputeBackgroundPlacement(Element* element, Vector2f area, Vector2f natural, bool no_intrinsic_size, BackgroundPlacement& out);

/// True if the element sets any of background-size, -position or -repeat to something other than the initial value.
bool HasBackgroundPlacement(Element* element);

/// Adds quads covering the area with the placed tiles, clipped to the area. With 'local_texcoords', texture coordinates are
/// pixel positions inside each tile (for gradient shaders); otherwise they interpolate between uv0 and uv1.
void GenerateBackgroundTiles(Mesh& mesh, Vector2f origin, Vector2f area, const BackgroundPlacement& placement, ColourbPremultiplied colour, Vector2f uv0,
	Vector2f uv1, bool local_texcoords);

} // namespace Rml
