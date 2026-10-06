#include "DecoratorTiledImage.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "CalcExpression.h"
#include "../../Include/RmlUi/Core/Context.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/RenderManager.h"

namespace Rml {

DecoratorTiledImage::DecoratorTiledImage() {}

DecoratorTiledImage::~DecoratorTiledImage() {}

bool DecoratorTiledImage::Initialise(const Tile& _tile, Texture _texture)
{
	tile = _tile;
	tile.texture_index = AddTexture(_texture);
	return (tile.texture_index >= 0);
}

// Forge: CSS background images in web pages, sized, positioned and repeated by background-size, -position and -repeat.
namespace {
	String BackgroundString(Element* element, const char* name)
	{
		const Property* property = element->GetProperty(name);
		return (property && (property->unit == Unit::STRING || property->unit == Unit::CALC) ? StringUtilities::ToLower(StringUtilities::StripWhitespace(property->Get<String>())) : String());
	}

	// Splits at spaces outside parentheses, so calc() expressions stay whole.
	StringList SplitSpaces(const String& value)
	{
		StringList result;
		String current;
		int depth = 0;
		for (char c : value)
		{
			if (c == '(')
				depth++;
			else if (c == ')')
				depth--;
			if (StringUtilities::IsWhitespace(c) && depth == 0)
			{
				if (!current.empty())
					result.push_back(std::move(current));
				current.clear();
			}
			else
				current += c;
		}
		if (!current.empty())
			result.push_back(std::move(current));
		return result;
	}

	// Resolves a length, percentage or math function; returns false for 'auto' and keywords.
	bool ResolveBackgroundLength(Element* element, const String& token, float percent_base, float& out_value)
	{
		if (token.empty() || token == "auto" || (std::isalpha((unsigned char)token[0]) && !Calc::ContainsMath(token)))
			return false;
		Calc::Context context;
		context.font_size = element->GetComputedValues().font_size();
		if (ElementDocument* document = element->GetOwnerDocument())
			context.root_font_size = document->GetComputedValues().font_size();
		if (Context* ui_context = element->GetContext())
		{
			context.dp_ratio = ui_context->GetDensityIndependentPixelRatio();
			context.viewport = Vector2f(ui_context->GetDimensions());
		}
		context.percent_base_guess = percent_base;
		const Calc::Result result = Calc::Resolve(Calc::ContainsMath(token) ? token : "calc(" + token + ")", context);
		if (!result.valid)
			return false;
		out_value = result.px + result.number + result.percent * 0.01f * percent_base;
		return true;
	}

	bool GenerateCssBackground(Mesh& mesh, Element* element, BoxArea paint_area, const DecoratorTiled::Tile& tile, Vector2f natural)
	{
		const RenderBox render_box = element->GetRenderBox(paint_area);
		const Vector2f origin = render_box.GetFillOffset();
		const Vector2f area = render_box.GetFillSize();
		if (area.x <= 0.f || area.y <= 0.f || natural.x <= 0.f || natural.y <= 0.f)
			return true;

		// Size.
		const StringList size_tokens = SplitSpaces(BackgroundString(element, "background-size"));
		const float ratio = natural.x / natural.y;
		Vector2f image = natural;
		const String first = (size_tokens.empty() ? String("auto") : size_tokens[0]);
		const bool all_auto = (first == "auto" && (size_tokens.size() < 2 || size_tokens[1] == "auto"));
		if (first == "contain" || first == "cover" || (all_auto && tile.vector_image))
		{
			const float scale_x = area.x / natural.x, scale_y = area.y / natural.y;
			const float scale = (first == "cover" ? Math::Max(scale_x, scale_y) : Math::Min(scale_x, scale_y));
			image = natural * scale;
		}
		else if (!all_auto)
		{
			float w = 0.f, h = 0.f;
			const bool has_w = ResolveBackgroundLength(element, first, area.x, w);
			const bool has_h = (size_tokens.size() >= 2 ? ResolveBackgroundLength(element, size_tokens[1], area.y, h) : false);
			if (has_w && has_h)
				image = {w, h};
			else if (has_w)
				image = {w, w / ratio};
			else if (has_h)
				image = {h * ratio, h};
		}
		if (image.x <= 0.f || image.y <= 0.f)
			return true;

		// Position: keywords, percentages and lengths, also with edge offsets such as 'right 0.75rem center'.
		const StringList position_tokens = SplitSpaces(BackgroundString(element, "background-position"));
		float position[2] = {0.f, 0.f};
		{
			struct Part {
				String keyword;
				String offset;
			};
			Vector<Part> parts;
			for (const String& token : position_tokens)
			{
				const bool is_keyword = (token == "left" || token == "right" || token == "top" || token == "bottom" || token == "center");
				if (!is_keyword && !parts.empty() && !parts.back().keyword.empty() && parts.back().offset.empty() && parts.back().keyword != "center")
					parts.back().offset = token;
				else
					parts.push_back(is_keyword ? Part{token, String()} : Part{String(), token});
			}
			if (parts.size() == 1)
			{
				if (parts[0].keyword == "top" || parts[0].keyword == "bottom")
					parts.insert(parts.begin(), Part{"center", String()});
				else
					parts.push_back(Part{"center", String()});
			}
			if (parts.size() >= 2 && (parts[0].keyword == "top" || parts[0].keyword == "bottom" || parts[1].keyword == "left" || parts[1].keyword == "right"))
				std::swap(parts[0], parts[1]);
			for (int axis = 0; axis < 2 && axis < (int)parts.size(); axis++)
			{
				const float free_space = (axis == 0 ? area.x - image.x : area.y - image.y);
				const float extent = (axis == 0 ? area.x : area.y);
				const Part& part = parts[axis];
				float offset = 0.f;
				const bool has_offset = !part.offset.empty() && ResolveBackgroundLength(element, part.offset, free_space, offset);
				if (part.keyword.empty())
					position[axis] = (has_offset ? offset : 0.f);
				else if (part.keyword == "center")
					position[axis] = 0.5f * free_space;
				else if (part.keyword == "left" || part.keyword == "top")
					position[axis] = (has_offset ? offset : 0.f);
				else
					position[axis] = free_space - (has_offset ? offset : 0.f);
				(void)extent;
			}
		}

		// Repeat.
		const String repeat = BackgroundString(element, "background-repeat");
		bool repeat_x = true, repeat_y = true;
		if (repeat == "no-repeat" || repeat == "no-repeat no-repeat")
			repeat_x = repeat_y = false;
		else if (repeat == "repeat-x" || repeat == "repeat no-repeat")
			repeat_y = false;
		else if (repeat == "repeat-y" || repeat == "no-repeat repeat")
			repeat_x = false;

		const ComputedValues& computed = element->GetComputedValues();
		const ColourbPremultiplied colour = computed.image_color().ToPremultiplied(computed.opacity());
		const Vector2f uv0 = tile.tile_data.texcoords[0], uv1 = tile.tile_data.texcoords[1];

		auto first_tile = [](float pos, float size, bool repeat) {
			if (!repeat)
				return pos;
			return pos - std::ceil(pos / size) * size;
		};
		const float start_x = first_tile(position[0], image.x, repeat_x);
		const float start_y = first_tile(position[1], image.y, repeat_y);
		const int max_tiles = 4096;
		int count = 0;
		for (float y = start_y; y < area.y && count < max_tiles; y += image.y)
		{
			for (float x = start_x; x < area.x && count < max_tiles; x += image.x)
			{
				// Clip the tile to the painting area.
				const Vector2f p0 = {Math::Max(x, 0.f), Math::Max(y, 0.f)};
				const Vector2f p1 = {Math::Min(x + image.x, area.x), Math::Min(y + image.y, area.y)};
				if (p1.x > p0.x && p1.y > p0.y)
				{
					const Vector2f t0 = uv0 + (uv1 - uv0) * ((p0 - Vector2f(x, y)) / image);
					const Vector2f t1 = uv0 + (uv1 - uv0) * ((p1 - Vector2f(x, y)) / image);
					MeshUtilities::GenerateQuad(mesh, origin + p0, p1 - p0, colour, t0, t1);
				}
				count++;
				if (!repeat_x)
					break;
			}
			if (!repeat_y)
				break;
		}
		return true;
	}
} // namespace

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
		GenerateCssBackground(mesh, element, paint_area, tile, tile.GetNaturalDimensions(element));
	else
		tile.GenerateGeometry(mesh, computed, offset, size, tile.GetNaturalDimensions(element));

	Geometry* data = new Geometry(element->GetRenderManager()->MakeGeometry(std::move(mesh)));

	return reinterpret_cast<DecoratorDataHandle>(data);
}

void DecoratorTiledImage::ReleaseElementData(DecoratorDataHandle element_data) const
{
	delete reinterpret_cast<Geometry*>(element_data);
}

void DecoratorTiledImage::RenderElement(Element* element, DecoratorDataHandle element_data) const
{
	Geometry* data = reinterpret_cast<Geometry*>(element_data);
	data->Render(element->GetAbsoluteOffset(BoxArea::Border), GetTexture());
}

DecoratorTiledImageInstancer::DecoratorTiledImageInstancer() : DecoratorTiledInstancer(1)
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

	return decorator;
}

} // namespace Rml
