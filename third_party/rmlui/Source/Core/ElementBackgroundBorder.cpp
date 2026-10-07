#include "ElementBackgroundBorder.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/Box.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/DecorationTypes.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "BoxShadowCache.h"
#include "GeometryBoxShadow.h"

namespace Rml {

ElementBackgroundBorder::ElementBackgroundBorder() {}

void ElementBackgroundBorder::Render(Element* element)
{
	if (background_dirty || border_dirty)
	{
		for (auto& background : backgrounds)
		{
			if (background.first != BackgroundType::BackgroundBorder)
				background.second.geometry.Release();
		}

		GenerateGeometry(element);
		GenerateOutline(element);

		background_dirty = false;
		border_dirty = false;
	}

	if (Background* shadow = GetBackground(BackgroundType::BoxShadowAndBackgroundBorder))
	{
		const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
		shadow->box_shadow_and_background_border->geometry.Render(offset, shadow->box_shadow_and_background_border->texture);
	}
	else if (Background* background = GetBackground(BackgroundType::BackgroundBorder))
	{
		const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
		background->geometry.Render(offset);
	}

	if (Background* outline = GetBackground(BackgroundType::Outline))
		outline->geometry.Render(element->GetAbsoluteOffset(BoxArea::Border));
}

void ElementBackgroundBorder::DirtyBackground()
{
	background_dirty = true;
}

void ElementBackgroundBorder::DirtyBorder()
{
	border_dirty = true;
}

Geometry* ElementBackgroundBorder::GetClipGeometry(Element* element, BoxArea clip_area)
{
	BackgroundType type = {};
	switch (clip_area)
	{
	case Rml::BoxArea::Border: type = BackgroundType::ClipBorder; break;
	case Rml::BoxArea::Padding: type = BackgroundType::ClipPadding; break;
	case Rml::BoxArea::Content: type = BackgroundType::ClipContent; break;
	default: RMLUI_ERROR; return nullptr;
	}

	RenderManager* render_manager = element->GetRenderManager();
	Geometry& geometry = GetOrCreateBackground(type).geometry;
	if (render_manager && !geometry)
	{
		Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);
		MeshUtilities::GenerateBackground(mesh, element->GetRenderBox(clip_area), ColourbPremultiplied(255));
		geometry = render_manager->MakeGeometry(std::move(mesh));
	}

	return &geometry;
}

ElementBackgroundBorder::Background* ElementBackgroundBorder::GetBackground(BackgroundType type)
{
	auto it = backgrounds.find(type);
	if (it != backgrounds.end())
		return &it->second;
	return nullptr;
}

ElementBackgroundBorder::Background& ElementBackgroundBorder::GetOrCreateBackground(BackgroundType type)
{
	auto it = backgrounds.find(type);
	if (it != backgrounds.end())
		return it->second;

	Background& background = backgrounds[type];
	return background;
}

void ElementBackgroundBorder::EraseBackground(BackgroundType type)
{
	backgrounds.erase(type);
}

void ElementBackgroundBorder::GenerateGeometry(Element* element)
{
	RMLUI_ZoneScoped;
	RenderManager* render_manager = element->GetRenderManager();
	if (!render_manager)
		return;

	const ComputedValues& computed = element->GetComputedValues();
	const bool has_box_shadow = computed.has_box_shadow();

	if (has_box_shadow)
	{
		// The box shadow geometry also includes the element's background and border, thus we can skip the normal background generation.
		EraseBackground(BackgroundType::BackgroundBorder);
		Background& shadow_background = GetOrCreateBackground(BackgroundType::BoxShadowAndBackgroundBorder);
		shadow_background.box_shadow_and_background_border = BoxShadowCache::GetHandle(element, computed);
		return;
	}

	EraseBackground(BackgroundType::BoxShadowAndBackgroundBorder);

	const float opacity = computed.opacity();
	ColourbPremultiplied background_color = computed.background_color().ToPremultiplied(opacity);
	Array<ColourbPremultiplied, 4> border_colors = {
		computed.border_top_color().ToPremultiplied(opacity),
		computed.border_right_color().ToPremultiplied(opacity),
		computed.border_bottom_color().ToPremultiplied(opacity),
		computed.border_left_color().ToPremultiplied(opacity),
	};

	// Forge: a border picture (border-image) takes the place of the border's own drawing.
	{
		static const PropertyId source_id = StyleSheetSpecification::GetPropertyId("border-image-source");
		const Property* source = (source_id != PropertyId::Invalid ? element->GetProperty(source_id) : nullptr);
		if (source && source->unit == Unit::STRING && StringUtilities::StartsWith(StringUtilities::ToLower(source->Get<String>()), "url("))
			for (ColourbPremultiplied& colour : border_colors)
				colour = ColourbPremultiplied(0, 0, 0, 0);
	}

	const uint8_t border_styles[4] = {uint8_t(computed.border_top_style()), uint8_t(computed.border_right_style()),
		uint8_t(computed.border_bottom_style()), uint8_t(computed.border_left_style())};

	Geometry& geometry = GetOrCreateBackground(BackgroundType::BackgroundBorder).geometry;
	Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);

	// Forge: as in a browser, the background of a web page's <html> (or else of its <body>) fills the whole window.
	if (element->GetTagName() == "html")
	{
		ColourbPremultiplied canvas = background_color;
		if (canvas.alpha == 0)
			for (int i = 0; i < element->GetNumChildren(); i++)
				if (Element* body = element->GetChild(i); body->GetTagName() == "body")
				{
					canvas = body->GetComputedValues().background_color().ToPremultiplied(opacity);
					break;
				}
		if (Context* context = element->GetContext(); context && canvas.alpha > 0)
			MeshUtilities::GenerateQuad(mesh, -element->GetAbsoluteOffset(BoxArea::Border),
				Vector2f(context->GetDimensions()) + Vector2f(1.f), canvas);
	}
	else if (element->GetTagName() == "body" && element->GetParentNode() && element->GetParentNode()->GetTagName() == "html" &&
		element->GetParentNode()->GetComputedValues().background_color().alpha == 0)
	{
		background_color = ColourbPremultiplied(0, 0, 0, 0); // drawn by <html> over the whole window
	}

	for (int i = 0; i < element->GetNumBoxes(); i++)
		MeshUtilities::GenerateBackgroundBorder(mesh, element->GetRenderBox(BoxArea::Padding, i), background_color, border_colors.data(),
			border_styles); // Forge: border styles

	geometry = render_manager->MakeGeometry(std::move(mesh));
}

void ElementBackgroundBorder::GenerateOutline(Element* element)
{
	RenderManager* render_manager = element->GetRenderManager();
	const Property* style_property = element->GetProperty(PropertyId::OutlineStyle);
	const int style = (style_property ? style_property->Get<int>() : 0);
	const bool visible = style_property && style != (int)Style::BorderStyle::None && style != (int)Style::BorderStyle::Hidden;
	const Property* width_property = element->GetProperty(PropertyId::OutlineWidth);
	const float width = (width_property ? Math::Round(element->ResolveLength(width_property->GetNumericValue())) : 0.f);
	if (!render_manager || !visible || width <= 0.f)
	{
		EraseBackground(BackgroundType::Outline);
		return;
	}

	const ComputedValues& computed = element->GetComputedValues();
	Colourb color = computed.color();
	if (const Property* color_property = element->GetProperty(PropertyId::OutlineColor); color_property && color_property->unit == Unit::COLOUR)
		color = color_property->Get<Colourb>();
	const ColourbPremultiplied premultiplied = color.ToPremultiplied(computed.opacity());
	const Property* offset_property = element->GetProperty(PropertyId::OutlineOffset);
	const float offset = (offset_property ? Math::Round(element->ResolveLength(offset_property->GetNumericValue())) : 0.f);

	const ColourbPremultiplied colors[4] = {premultiplied, premultiplied, premultiplied, premultiplied};
	// 'auto' outlines (focus rings) are drawn solid.
	const uint8_t outline_style = (style == (int)Style::BorderStyle::Auto ? (uint8_t)Style::BorderStyle::Solid : (uint8_t)style);
	const uint8_t styles[4] = {outline_style, outline_style, outline_style, outline_style};

	Geometry& geometry = GetOrCreateBackground(BackgroundType::Outline).geometry;
	Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);
	for (int i = 0; i < element->GetNumBoxes(); i++)
	{
		const RenderBox box = element->GetRenderBox(BoxArea::Border, i);
		const float grow = offset + width;
		const Vector2f fill_size = box.GetFillSize() + Vector2f(2.f * offset);
		if (fill_size.x < 0.f || fill_size.y < 0.f)
			continue;
		CornerSizes radius = box.GetBorderRadius();
		for (float& r : radius)
			r = (r > 0.f ? Math::Max(r + grow, 0.f) : 0.f);
		const RenderBox outline_box(fill_size, box.GetBorderOffset() - Vector2f(grow), EdgeSizes{width, width, width, width}, radius);
		MeshUtilities::GenerateBackgroundBorder(mesh, outline_box, ColourbPremultiplied(0, 0, 0, 0), colors, styles);
	}
	geometry = render_manager->MakeGeometry(std::move(mesh));
}

} // namespace Rml
