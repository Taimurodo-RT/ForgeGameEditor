#include "ElementBorderImage.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/DataUri.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Mesh.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "BackgroundPlacement.h"
#include <cmath>
#include <cstdlib>

namespace Rml {

namespace {

	// The picture's path from 'url(...)', unquoted; empty for 'none' and anything else.
	String SourcePath(const String& value)
	{
		const String v = StringUtilities::StripWhitespace(value);
		if (!StringUtilities::StartsWith(StringUtilities::ToLower(v), "url(") || v.back() != ')')
			return String();
		String path = StringUtilities::StripWhitespace(v.substr(4, v.size() - 5));
		if (path.size() >= 2 && (path[0] == '"' || path[0] == '\''))
			path = path.substr(1, path.size() - 2);
		return path;
	}

	// Up to four values for top, right, bottom, left, as in CSS box shorthands.
	Array<String, 4> FourSides(const StringList& tokens)
	{
		Array<String, 4> out;
		const size_t n = tokens.size();
		for (int i = 0; i < 4; i++)
		{
			const size_t index = (n == 0 ? 0 : n == 1 ? 0 : n == 2 ? (size_t)(i % 2) : n == 3 ? (i == 3 ? 1 : (size_t)i) : (size_t)i);
			out[i] = (n == 0 ? String() : tokens[index]);
		}
		return out;
	}

	bool IsUnitless(const String& token)
	{
		return !token.empty() && token.find_first_not_of("+-.0123456789eE") == String::npos;
	}

	enum class Repeat { Stretch, Repeat, Round, Space };

	Repeat ToRepeat(const String& word)
	{
		if (word == "repeat")
			return Repeat::Repeat;
		if (word == "round")
			return Repeat::Round;
		if (word == "space")
			return Repeat::Space;
		return Repeat::Stretch;
	}

	// Lays tiles of size 'tile' along a destination span [d0, d0 + length): the positions and visible parts of each tile, as
	// fractions of the tile (0..1).
	struct TileSpan {
		float pos, size;  // destination
		float t0, t1;     // part of the tile shown
	};
	Vector<TileSpan> Tiles(float length, float tile, Repeat repeat)
	{
		Vector<TileSpan> out;
		if (length <= 0.f)
			return out;
		if (repeat == Repeat::Stretch || tile <= 0.f)
		{
			out.push_back({0.f, length, 0.f, 1.f});
			return out;
		}
		const int max_tiles = 512;
		if (repeat == Repeat::Round)
		{
			const int n = Math::Clamp((int)std::lround(length / tile), 1, max_tiles);
			const float size = length / (float)n;
			for (int i = 0; i < n; i++)
				out.push_back({i * size, size, 0.f, 1.f});
			return out;
		}
		if (repeat == Repeat::Space)
		{
			const int n = Math::Min((int)std::floor(length / tile), max_tiles);
			if (n <= 0)
				return out;
			const float gap = (length - n * tile) / (float)(n + 1);
			for (int i = 0; i < n; i++)
				out.push_back({gap + i * (tile + gap), tile, 0.f, 1.f});
			return out;
		}
		// Repeat: tiles centred on the span, cut at both ends.
		const float start = length * 0.5f - tile * 0.5f - std::ceil((length * 0.5f - tile * 0.5f) / tile) * tile;
		for (float x = start; x < length && (int)out.size() < max_tiles; x += tile)
		{
			const float a = Math::Max(x, 0.f), b = Math::Min(x + tile, length);
			if (b > a)
				out.push_back({a, b - a, (a - x) / tile, (b - x) / tile});
		}
		return out;
	}

} // namespace

bool ElementBorderImage::Instance(Element* element)
{
	Release();
	const Property* property = element->GetProperty("border-image-source");
	if (!property || property->unit != Unit::STRING)
		return false;
	String path = SourcePath(property->Get<String>());
	if (path.empty())
		return false;
	if (StringUtilities::StartsWith(StringUtilities::ToLower(path), "data:"))
		path = DataUri::Register(path);
	RenderManager* render_manager = element->GetRenderManager();
	if (!render_manager)
		return false;
	String document_path;
	if (property->source)
		document_path = property->source->path;
	else if (ElementDocument* document = element->GetOwnerDocument())
		document_path = document->GetSourceURL();
	texture = render_manager->LoadTexture(path, document_path);
	active = (bool)texture;
	return active;
}

void ElementBorderImage::Release()
{
	geometry = Geometry();
	texture = Texture();
	active = false;
}

void ElementBorderImage::Generate(Element* element)
{
	geometry = Geometry();
	if (!active)
		return;
	const Vector2i dimensions = texture.GetDimensions();
	if (dimensions.x <= 0 || dimensions.y <= 0)
		return;
	const Vector2f image(dimensions);
	const Box& box = element->GetBox();
	const Vector2f border_size = box.GetSize(BoxArea::Border);
	const float border[4] = {box.GetEdge(BoxArea::Border, BoxEdge::Top), box.GetEdge(BoxArea::Border, BoxEdge::Right),
		box.GetEdge(BoxArea::Border, BoxEdge::Bottom), box.GetEdge(BoxArea::Border, BoxEdge::Left)};

	// Slices (picture pixels), with 'fill'.
	bool fill = false;
	StringList slice_tokens;
	for (const String& token : PlacementTokens(PlacementString(element, "border-image-slice")))
	{
		if (token == "fill")
			fill = true;
		else
			slice_tokens.push_back(token);
	}
	const Array<String, 4> slice_text = FourSides(slice_tokens);
	float slice[4];
	for (int i = 0; i < 4; i++)
	{
		const float base = (i % 2 == 0 ? image.y : image.x);
		const String& t = slice_text[i];
		float value = base;
		if (IsUnitless(t))
			value = (float)std::atof(t.c_str());
		else if (!t.empty() && t.back() == '%')
			value = (float)std::atof(t.c_str()) * 0.01f * base;
		slice[i] = Math::Clamp(value, 0.f, base);
	}

	// Outset, then the area the picture covers.
	const Array<String, 4> outset_text = FourSides(PlacementTokens(PlacementString(element, "border-image-outset")));
	float outset[4];
	for (int i = 0; i < 4; i++)
	{
		const String& t = outset_text[i];
		float value = 0.f;
		if (IsUnitless(t))
			value = (float)std::atof(t.c_str()) * border[i];
		else if (!ResolvePlacementLength(element, t, 0.f, value))
			value = 0.f;
		outset[i] = Math::Max(value, 0.f);
	}
	const Vector2f area_origin(-outset[3], -outset[0]);
	const Vector2f area(border_size.x + outset[1] + outset[3], border_size.y + outset[0] + outset[2]);
	if (area.x <= 0.f || area.y <= 0.f)
		return;

	// Widths of the border parts.
	const Array<String, 4> width_text = FourSides(PlacementTokens(PlacementString(element, "border-image-width")));
	float width[4];
	for (int i = 0; i < 4; i++)
	{
		const String& t = width_text[i];
		float value = border[i];
		if (t == "auto")
			value = slice[i];
		else if (IsUnitless(t))
			value = (float)std::atof(t.c_str()) * border[i];
		else if (!t.empty())
			ResolvePlacementLength(element, t, i % 2 == 0 ? area.y : area.x, value);
		width[i] = Math::Max(value, 0.f);
	}
	// Opposite parts that do not fit are scaled down together.
	const float fit = Math::Min(1.f, Math::Min(width[1] + width[3] > 0.f ? area.x / (width[1] + width[3]) : 1.f,
										 width[0] + width[2] > 0.f ? area.y / (width[0] + width[2]) : 1.f));
	for (float& w : width)
		w *= fit;

	StringList repeat_tokens = PlacementTokens(PlacementString(element, "border-image-repeat"));
	const Repeat repeat_x = ToRepeat(repeat_tokens.empty() ? String() : repeat_tokens[0]);
	const Repeat repeat_y = ToRepeat(repeat_tokens.size() >= 2 ? repeat_tokens[1] : repeat_tokens.empty() ? String() : repeat_tokens[0]);

	// Columns and rows: picture pixels (x0..x3) and destination pixels (d0..d3).
	const float sx[4] = {0.f, slice[3], image.x - slice[1], image.x};
	const float sy[4] = {0.f, slice[0], image.y - slice[2], image.y};
	const float dx[4] = {0.f, width[3], area.x - width[1], area.x};
	const float dy[4] = {0.f, width[0], area.y - width[2], area.y};

	const ComputedValues& computed = element->GetComputedValues();
	const ColourbPremultiplied colour = computed.image_color().ToPremultiplied(computed.opacity());
	Mesh mesh;

	auto part = [&](int col, int row) {
		const float dest_w = dx[col + 1] - dx[col], dest_h = dy[row + 1] - dy[row];
		const float src_w = sx[col + 1] - sx[col], src_h = sy[row + 1] - sy[row];
		if (dest_w <= 0.f || dest_h <= 0.f || src_w <= 0.f || src_h <= 0.f)
			return;
		// A tile's size: edges keep the picture's proportions scaled to the border width; the middle follows the top and
		// left edges.
		float tile_w = dest_w, tile_h = dest_h;
		if (col == 1)
		{
			const float scale = (row == 1 ? (sy[1] > 0.f ? width[0] / sy[1] : (sy[3] - sy[2] > 0.f ? width[2] / (sy[3] - sy[2]) : 1.f))
										  : dest_h / src_h);
			tile_w = src_w * scale;
		}
		if (row == 1)
		{
			const float scale = (col == 1 ? (sx[1] > 0.f ? width[3] / sx[1] : (sx[3] - sx[2] > 0.f ? width[1] / (sx[3] - sx[2]) : 1.f))
										  : dest_w / src_w);
			tile_h = src_h * scale;
		}
		const Vector<TileSpan> xs = (col == 1 ? Tiles(dest_w, tile_w, repeat_x) : Vector<TileSpan>{{0.f, dest_w, 0.f, 1.f}});
		const Vector<TileSpan> ys = (row == 1 ? Tiles(dest_h, tile_h, repeat_y) : Vector<TileSpan>{{0.f, dest_h, 0.f, 1.f}});
		for (const TileSpan& y : ys)
			for (const TileSpan& x : xs)
			{
				// Repeated parts keep half a picture pixel away from their neighbours, so no seams show between tiles.
				const float ix = (col == 1 && repeat_x != Repeat::Stretch ? Math::Min(0.5f, src_w * 0.25f) : 0.f);
				const float iy = (row == 1 && repeat_y != Repeat::Stretch ? Math::Min(0.5f, src_h * 0.25f) : 0.f);
				const float u0 = sx[col] + ix, u1 = sx[col + 1] - ix, v0 = sy[row] + iy, v1 = sy[row + 1] - iy;
				const Vector2f uv0((u0 + x.t0 * (u1 - u0)) / image.x, (v0 + y.t0 * (v1 - v0)) / image.y);
				const Vector2f uv1((u0 + x.t1 * (u1 - u0)) / image.x, (v0 + y.t1 * (v1 - v0)) / image.y);
				MeshUtilities::GenerateQuad(mesh, area_origin + Vector2f(dx[col] + x.pos, dy[row] + y.pos), Vector2f(x.size, y.size), colour, uv0, uv1);
			}
	};
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
			if (fill || row != 1 || col != 1)
				part(col, row);

	if (!mesh.indices.empty())
		geometry = element->GetRenderManager()->MakeGeometry(std::move(mesh));
}

void ElementBorderImage::Render(Element* element)
{
	if (active && geometry)
		geometry.Render(element->GetAbsoluteOffset(BoxArea::Border), texture);
}

} // namespace Rml
