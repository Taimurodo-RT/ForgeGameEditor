#include "DecoratorTiledImage.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "BackgroundPlacement.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#ifdef RMLUI_SVG_PLUGIN
	#include "../SVG/SVGCache.h"
#endif

namespace Rml {

namespace {
	// Forge: an element's tiles, and for an SVG picture its texture at the tiles' size on screen.
	struct TiledImageData {
		Geometry geometry;
#ifdef RMLUI_SVG_PLUGIN
		SharedPtr<SVG::SVGData> svg;
#endif
	};
} // namespace

DecoratorTiledImage::DecoratorTiledImage() {}

DecoratorTiledImage::~DecoratorTiledImage() {}

bool DecoratorTiledImage::Initialise(const Tile& _tile, Texture _texture)
{
	tile = _tile;
	tile.texture_index = AddTexture(_texture);
	return (tile.texture_index >= 0);
}

DecoratorDataHandle DecoratorTiledImage::GenerateElementData(Element* element, BoxArea paint_area) const
{
	// Calculate the tile's dimensions for this element.
	tile.CalculateDimensions(GetTexture());

	const ComputedValues& computed = element->GetComputedValues();

	const RenderBox render_box = element->GetRenderBox(paint_area);
	const Vector2f offset = render_box.GetFillOffset();
	const Vector2f size = render_box.GetFillSize();

	// Generate the geometry for the tile.
	Mesh mesh;
	ElementDocument* document = element->GetOwnerDocument();
	if (document && document->GetTagName() == "html")
	{
		// Forge: CSS background images, sized, positioned and repeated by background-size, -position and -repeat.
		BackgroundPlacement placement;
		const Vector2f natural = tile.GetNaturalDimensions(element);
		if (ComputeBackgroundPlacement(element, size, natural, tile.vector_image, placement, mask ? "mask" : "background"))
			GenerateBackgroundTiles(mesh, offset, size, placement, computed.image_color().ToPremultiplied(computed.opacity()), tile.tile_data.texcoords[0],
				tile.tile_data.texcoords[1], false);
	}
	else
		tile.GenerateGeometry(mesh, computed, offset, size, tile.GetNaturalDimensions(element));

	TiledImageData* data = new TiledImageData;
#ifdef RMLUI_SVG_PLUGIN
	// Forge: an SVG picture is rasterized at the size its tiles take on screen, and the tiles are put on whole pixels of
	// the window (RenderElement rounds the element's offset), so each pixel of the picture is a pixel of the window.
	if (tile.vector_image && !tile.vector_path.empty() && mesh.vertices.size() >= 4)
	{
		for (Vertex& vertex : mesh.vertices)
			vertex.position = vertex.position.Round();
		// A tile's size: the first quad's size over the part of the picture it shows (a quad cut at an edge shows less).
		const Vertex& a = mesh.vertices[0];
		const Vertex& b = mesh.vertices[2];
		const Vector2f shown = (b.tex_coord - a.tex_coord), drawn = (b.position - a.position);
		const Vector2f tile_size(shown.x != 0.f ? Math::Absolute(drawn.x / shown.x) : 0.f, shown.y != 0.f ? Math::Absolute(drawn.y / shown.y) : 0.f);
		data->svg = SVG::SVGCache::GetHandle(tile.vector_path, element, Vector2i(tile_size.Round()));
	}
#endif
	data->geometry = element->GetRenderManager()->MakeGeometry(std::move(mesh));

	return reinterpret_cast<DecoratorDataHandle>(data);
}

void DecoratorTiledImage::ReleaseElementData(DecoratorDataHandle element_data) const
{
	delete reinterpret_cast<TiledImageData*>(element_data);
}

void DecoratorTiledImage::RenderElement(Element* element, DecoratorDataHandle element_data) const
{
	TiledImageData* data = reinterpret_cast<TiledImageData*>(element_data);
#ifdef RMLUI_SVG_PLUGIN
	if (data->svg)
	{
		data->geometry.Render(element->GetAbsoluteOffset(BoxArea::Border).Round(), data->svg->texture);
		return;
	}
#endif
	data->geometry.Render(element->GetAbsoluteOffset(BoxArea::Border), GetTexture());
}

DecoratorTiledImageInstancer::DecoratorTiledImageInstancer(bool mask) : DecoratorTiledInstancer(1), mask(mask)
{
	RegisterTileProperty("image", true);
	RegisterShorthand("decorator", "image", ShorthandType::RecursiveRepeat);
}

DecoratorTiledImageInstancer::~DecoratorTiledImageInstancer() {}

SharedPtr<Decorator> DecoratorTiledImageInstancer::InstanceDecorator(const String& /*name*/, const PropertyDictionary& properties,
	const DecoratorInstancerInterface& instancer_interface)
{
	DecoratorTiled::Tile tile;
	Texture texture;

	if (!GetTileProperties(&tile, &texture, 1, properties, instancer_interface))
		return nullptr;

	auto decorator = MakeShared<DecoratorTiledImage>();

	if (!decorator->Initialise(tile, texture))
		return nullptr;
	decorator->SetMask(mask);

	return decorator;
}

} // namespace Rml
