#include "BackgroundPlacement.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Mesh.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "CalcExpression.h"
#include <cctype>
#include <cmath>
#include <utility>

namespace Rml {

namespace {

	// The value of a background placement property; of a comma-separated list (one value per layer), the first value.
	String BackgroundString(Element* element, const char* name)
	{
		const Property* property = element->GetProperty(name);
		if (!property || (property->unit != Unit::STRING && property->unit != Unit::CALC))
			return String();
		String value = StringUtilities::ToLower(StringUtilities::StripWhitespace(property->Get<String>()));
		int depth = 0;
		for (size_t i = 0; i < value.size(); i++)
		{
			if (value[i] == '(')
				depth++;
			else if (value[i] == ')')
				depth--;
			else if (value[i] == ',' && depth == 0)
				return StringUtilities::StripWhitespace(value.substr(0, i));
		}
		return value;
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

} // namespace

bool HasBackgroundPlacement(Element* element)
{
	const String size = BackgroundString(element, "background-size");
	const String position = BackgroundString(element, "background-position");
	const String repeat = BackgroundString(element, "background-repeat");
	return !(size.empty() || size == "auto" || size == "auto auto") || !(position.empty() || position == "0% 0%" || position == "0 0") ||
		!(repeat.empty() || repeat == "repeat");
}

bool ComputeBackgroundPlacement(Element* element, Vector2f area, Vector2f natural, bool no_intrinsic_size, BackgroundPlacement& out)
{
	if (area.x <= 0.f || area.y <= 0.f)
		return false;
	const bool fill_area = (natural.x <= 0.f || natural.y <= 0.f);
	if (fill_area)
		natural = area;

	// Size.
	const StringList size_tokens = SplitSpaces(BackgroundString(element, "background-size"));
	const float ratio = natural.x / natural.y;
	Vector2f image = natural;
	const String first = (size_tokens.empty() ? String("auto") : size_tokens[0]);
	const bool all_auto = (first == "auto" && (size_tokens.size() < 2 || size_tokens[1] == "auto"));
	if (first == "contain" || first == "cover" || (all_auto && no_intrinsic_size && !fill_area))
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
		return false;

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

	out.image_size = image;
	out.position = {position[0], position[1]};
	out.repeat_x = repeat_x;
	out.repeat_y = repeat_y;
	return true;
}

void GenerateBackgroundTiles(Mesh& mesh, Vector2f origin, Vector2f area, const BackgroundPlacement& placement, ColourbPremultiplied colour, Vector2f uv0,
	Vector2f uv1, bool local_texcoords)
{
	const Vector2f image = placement.image_size;
	if (image.x <= 0.f || image.y <= 0.f)
		return;
	auto first_tile = [](float pos, float size, bool repeat) {
		if (!repeat)
			return pos;
		return pos - std::ceil(pos / size) * size;
	};
	const float start_x = first_tile(placement.position.x, image.x, placement.repeat_x);
	const float start_y = first_tile(placement.position.y, image.y, placement.repeat_y);
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
				Vector2f t0, t1;
				if (local_texcoords)
				{
					t0 = p0 - Vector2f(x, y);
					t1 = p1 - Vector2f(x, y);
				}
				else
				{
					t0 = uv0 + (uv1 - uv0) * ((p0 - Vector2f(x, y)) / image);
					t1 = uv0 + (uv1 - uv0) * ((p1 - Vector2f(x, y)) / image);
				}
				MeshUtilities::GenerateQuad(mesh, origin + p0, p1 - p0, colour, t0, t1);
			}
			count++;
			if (!placement.repeat_x)
				break;
		}
		if (!placement.repeat_y)
			break;
	}
}

} // namespace Rml
