#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Box.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "GeometryBackgroundBorder.h"
#include "../../Include/RmlUi/Core/Mesh.h"
#include "../../Include/RmlUi/Core/StyleTypes.h"
#include <cmath>

namespace Rml {

void MeshUtilities::GenerateQuad(Mesh& mesh, Vector2f origin, Vector2f dimensions, ColourbPremultiplied colour)
{
	GenerateQuad(mesh, origin, dimensions, colour, Vector2f(0, 0), Vector2f(1, 1));
}

void MeshUtilities::GenerateQuad(Mesh& mesh, Vector2f origin, Vector2f dimensions, ColourbPremultiplied colour, Vector2f top_left_texcoord,
	Vector2f bottom_right_texcoord)
{
	const int v0 = (int)mesh.vertices.size();
	const int i0 = (int)mesh.indices.size();

	mesh.vertices.resize(mesh.vertices.size() + 4);
	mesh.indices.resize(mesh.indices.size() + 6);
	Vertex* vertices = mesh.vertices.data();
	int* indices = mesh.indices.data();

	vertices[v0 + 0].position = origin;
	vertices[v0 + 0].colour = colour;
	vertices[v0 + 0].tex_coord = top_left_texcoord;

	vertices[v0 + 1].position = Vector2f(origin.x + dimensions.x, origin.y);
	vertices[v0 + 1].colour = colour;
	vertices[v0 + 1].tex_coord = Vector2f(bottom_right_texcoord.x, top_left_texcoord.y);

	vertices[v0 + 2].position = origin + dimensions;
	vertices[v0 + 2].colour = colour;
	vertices[v0 + 2].tex_coord = bottom_right_texcoord;

	vertices[v0 + 3].position = Vector2f(origin.x, origin.y + dimensions.y);
	vertices[v0 + 3].colour = colour;
	vertices[v0 + 3].tex_coord = Vector2f(top_left_texcoord.x, bottom_right_texcoord.y);

	indices[i0 + 0] = v0 + 0;
	indices[i0 + 1] = v0 + 3;
	indices[i0 + 2] = v0 + 1;

	indices[i0 + 3] = v0 + 1;
	indices[i0 + 4] = v0 + 3;
	indices[i0 + 5] = v0 + 2;
}

void MeshUtilities::GenerateLine(Mesh& mesh, Vector2f position, Vector2f size, ColourbPremultiplied color)
{
	Math::SnapToPixelGrid(position, size);
	MeshUtilities::GenerateQuad(mesh, position, size, color);
}

void MeshUtilities::GenerateBackgroundBorder(Mesh& out_mesh, const RenderBox& render_box, ColourbPremultiplied background_color,
	const ColourbPremultiplied border_colors[4])
{
	RMLUI_ASSERT(border_colors);

	Vector<Vertex>& vertices = out_mesh.vertices;
	Vector<int>& indices = out_mesh.indices;

	const EdgeSizes& border_widths = render_box.GetBorderWidths();
	int num_borders = 0;
	for (int i = 0; i < 4; i++)
		if (border_colors[i].alpha > 0 && border_widths[i] > 0)
			num_borders += 1;

	const Vector2f fill_size = render_box.GetFillSize();
	const bool has_background = (background_color.alpha > 0 && fill_size.x > 0 && fill_size.y > 0);
	const bool has_border = (num_borders > 0);

	if (!has_background && !has_border)
		return;

	// Reserve geometry. A conservative estimate, does not take border-radii into account and assumes same-colored borders.
	const int estimated_num_vertices = 4 * int(has_background) + 2 * num_borders;
	const int estimated_num_triangles = 2 * int(has_background) + 2 * num_borders;
	vertices.reserve((int)vertices.size() + estimated_num_vertices);
	indices.reserve((int)indices.size() + 3 * estimated_num_triangles);

	// Generate the geometry.
	GeometryBackgroundBorder geometry(vertices, indices);
	const BorderMetrics metrics =
		GeometryBackgroundBorder::ComputeBorderMetrics(render_box.GetBorderOffset(), border_widths, fill_size, render_box.GetBorderRadius());

	if (has_background)
		geometry.DrawBackground(metrics, background_color);

	if (has_border)
		geometry.DrawBorder(metrics, border_widths, border_colors);

#if 0
	// Debug draw vertices
	if (render_box.border_radius != CornerSizes{})
	{
		const int num_vertices = (int)vertices.size();
		const int num_indices = (int)indices.size();
		vertices.reserve(num_vertices + 4 * num_vertices);
		indices.reserve(num_indices + 6 * num_indices);

		for (int i = 0; i < num_vertices; i++)
			MeshUtilities::GenerateQuad(out_mesh, vertices[i].position, Vector2f(3, 3), ColourbPremultiplied(255, 0, (i % 2) == 0 ? 0 : 255));
	}
#endif

#ifdef RMLUI_DEBUG
	const int num_vertices = (int)vertices.size();
	for (int index : indices)
	{
		RMLUI_ASSERT(index < num_vertices);
	}
#endif
}

// Forge: the darker shade a browser uses for inset, outset, groove and ridge borders (as Chromium's Color::Dark()).
static ColourbPremultiplied DarkShade(ColourbPremultiplied color)
{
	if (color.alpha == 0)
		return color;
	// Work on the unpremultiplied value.
	const float a = color.alpha / 255.f;
	const float r = color.red / 255.f / a, g = color.green / 255.f / a, b = color.blue / 255.f / a;
	const float v = Math::Max(r, Math::Max(g, b));
	float dr = 0.329f, dg = 0.329f, db = 0.329f; // black becomes dark grey
	if (v > 0.f)
	{
		const float multiplier = Math::Max(0.f, (v - 0.33f) / v);
		dr = r * multiplier, dg = g * multiplier, db = b * multiplier;
	}
	auto to_byte = [a](float c) { return (Rml::byte)Math::Clamp(int(c * a * 255.f + 0.5f), 0, 255); };
	return ColourbPremultiplied(to_byte(dr), to_byte(dg), to_byte(db), color.alpha);
}

// Forge: appends a quad with the given corners (clockwise).
static void AppendQuad(Mesh& mesh, Vector2f p0, Vector2f p1, Vector2f p2, Vector2f p3, ColourbPremultiplied color)
{
	const int v0 = (int)mesh.vertices.size();
	for (Vector2f p : {p0, p1, p2, p3})
	{
		Vertex vertex;
		vertex.position = p;
		vertex.colour = color;
		vertex.tex_coord = Vector2f(0.f);
		mesh.vertices.push_back(vertex);
	}
	for (int i : {0, 3, 1, 1, 3, 2})
		mesh.indices.push_back(v0 + i);
}

// Forge: dashes or dots along one edge, following the rounded corners: the path runs along the middle of the border from the middle of
// one corner arc to the middle of the next.
static void GenerateDashedEdge(Mesh& mesh, int edge, bool dotted, const RenderBox& render_box, ColourbPremultiplied color)
{
	const EdgeSizes widths = render_box.GetBorderWidths();
	const float w = widths[edge];
	if (w <= 0.f || color.alpha == 0)
		return;
	const Vector2f origin = render_box.GetBorderOffset();
	const Vector2f size = render_box.GetFillSize() + Vector2f(widths[1] + widths[3], widths[0] + widths[2]);
	CornerSizes radii = render_box.GetBorderRadius();
	const float max_radius = 0.5f * Math::Min(size.x, size.y);
	for (float& r : radii)
		r = Math::Clamp(r, 0.f, max_radius);

	// Corners: 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left. Edge e runs clockwise from corner e to corner e+1 (edge 3: 3 -> 0).
	const Vector2f corner_points[4] = {origin, origin + Vector2f(size.x, 0.f), origin + size, origin + Vector2f(0.f, size.y)};
	const Vector2f inward[4] = {Vector2f(1, 1), Vector2f(-1, 1), Vector2f(-1, -1), Vector2f(1, -1)};
	const float start_angle[4] = {180.f, 270.f, 0.f, 90.f}; // Angle at which each corner's arc begins, going clockwise.
	const int corner_a = edge, corner_b = (edge + 1) % 4;

	Vector<Vector2f> path;
	auto add_arc = [&](int corner, float a0, float a1) {
		const float r = radii[corner];
		const Vector2f center = corner_points[corner] + inward[corner] * r;
		const float centerline = Math::Max(0.f, r - 0.5f * w);
		if (r <= 0.5f * w)
		{
			// Sharp corner: the path starts or ends at the outer corner, so dashes cover it.
			const Vector2f normal_offset = inward[corner] * 0.5f * w;
			Vector2f point = corner_points[corner];
			if (edge % 2 == 0)
				point.y += normal_offset.y;
			else
				point.x += normal_offset.x;
			path.push_back(point);
			return;
		}
		const int steps = Math::Max(2, int(r * 0.5f));
		for (int i = 0; i <= steps; i++)
		{
			const float angle = Math::DegreesToRadians(a0 + (a1 - a0) * float(i) / float(steps));
			path.push_back(center + Vector2f(std::cos(angle), std::sin(angle)) * centerline);
		}
	};
	add_arc(corner_a, start_angle[corner_a] + 45.f, start_angle[corner_a] + 90.f);
	add_arc(corner_b, start_angle[corner_b], start_angle[corner_b] + 45.f);
	// Sharp corners of the vertical edges stop at the horizontal borders, which cover the corner squares.
	if (edge % 2 == 1)
	{
		if (radii[corner_a] <= 0.5f * w)
			path.front().y += (edge == 1 ? widths[0] : -widths[2]);
		if (radii[corner_b] <= 0.5f * w)
			path.back().y += (edge == 1 ? -widths[2] : widths[0]);
	}

	Vector<float> lengths(path.size(), 0.f);
	for (size_t i = 1; i < path.size(); i++)
		lengths[i] = lengths[i - 1] + (path[i] - path[i - 1]).Magnitude();
	const float total = lengths.back();
	if (total <= 0.f)
		return;

	// Chromium's pattern: dashes of 2w (3w when thinner than 3px) with gaps of w (2w), dots and gaps of w; stretched to end on a dash.
	float dash = (dotted ? w : (w >= 3.f ? 2.f * w : 3.f * w));
	float gap = (dotted ? w : (w >= 3.f ? w : 2.f * w));
	int count = Math::Max(1, int(std::round((total + gap) / (dash + gap))));
	if (count > 1)
		gap = (total - float(count) * dash) / float(count - 1);
	else
		dash = total;
	if (gap < 0.f)
	{
		gap = 0.f;
		dash = total / float(count);
	}

	const float half = 0.5f * w;
	for (int n = 0; n < count; n++)
	{
		const float s0 = float(n) * (dash + gap);
		const float s1 = Math::Min(total, s0 + dash);
		for (size_t i = 1; i < path.size(); i++)
		{
			const float a = Math::Max(s0, lengths[i - 1]), b = Math::Min(s1, lengths[i]);
			const float segment = lengths[i] - lengths[i - 1];
			if (b <= a || segment <= 0.f)
				continue;
			const Vector2f direction = (path[i] - path[i - 1]) / segment;
			const Vector2f p0 = path[i - 1] + direction * (a - lengths[i - 1]);
			const Vector2f p1 = path[i - 1] + direction * (b - lengths[i - 1]);
			const Vector2f normal = Vector2f(-direction.y, direction.x) * half;
			AppendQuad(mesh, p0 - normal, p1 - normal, p1 + normal, p0 + normal, color);
		}
	}
}

void MeshUtilities::GenerateBackgroundBorder(Mesh& out_mesh, const RenderBox& render_box, ColourbPremultiplied background_color,
	const ColourbPremultiplied border_colors[4], const uint8_t border_styles[4])
{
	using Style::BorderStyle;
	bool all_solid = true;
	for (int i = 0; i < 4; i++)
	{
		const BorderStyle style = BorderStyle(border_styles[i]);
		all_solid &= (style == BorderStyle::Auto || style == BorderStyle::Solid || style == BorderStyle::None || style == BorderStyle::Hidden);
	}
	if (all_solid)
	{
		GenerateBackgroundBorder(out_mesh, render_box, background_color, border_colors);
		return;
	}

	const ColourbPremultiplied transparent(0, 0, 0, 0);
	const EdgeSizes widths = render_box.GetBorderWidths();

	// Solid-looking edges, including inset and outset with their shades, drawn together with the background.
	ColourbPremultiplied solid[4];
	bool has_bands = false;
	for (int i = 0; i < 4; i++)
	{
		const BorderStyle style = BorderStyle(border_styles[i]);
		const bool top_left = (i == 0 || i == 3);
		switch (style)
		{
		case BorderStyle::Inset: solid[i] = (top_left ? DarkShade(border_colors[i]) : border_colors[i]); break;
		case BorderStyle::Outset: solid[i] = (top_left ? border_colors[i] : DarkShade(border_colors[i])); break;
		case BorderStyle::Dashed:
		case BorderStyle::Dotted: solid[i] = transparent; break;
		case BorderStyle::Double:
		case BorderStyle::Groove:
		case BorderStyle::Ridge:
			solid[i] = transparent;
			has_bands = true;
			break;
		default: solid[i] = border_colors[i]; break;
		}
	}
	GenerateBackgroundBorder(out_mesh, render_box, background_color, solid);

	// Double, groove and ridge: the border is drawn as bands, each a part [t0, t1] of the border width from the outer edge.
	if (has_bands)
	{
		auto draw_band = [&](float t0, float t1, const ColourbPremultiplied colors[4]) {
			const Vector2f outer = render_box.GetBorderOffset() + Vector2f(widths[3], widths[0]) * t0;
			EdgeSizes band;
			for (int i = 0; i < 4; i++)
				band[i] = widths[i] * (t1 - t0);
			const Vector2f inner_size = render_box.GetFillSize() + Vector2f(widths[1] + widths[3], widths[0] + widths[2]) * (1.f - t1);
			CornerSizes radii = render_box.GetBorderRadius();
			for (int c = 0; c < 4; c++)
			{
				const float horizontal = widths[c < 2 ? 0 : 2], vertical = widths[(c == 0 || c == 3) ? 3 : 1];
				radii[c] = Math::Max(0.f, radii[c] - 0.5f * (horizontal + vertical) * t0);
			}
			const BorderMetrics metrics = GeometryBackgroundBorder::ComputeBorderMetrics(outer, band, inner_size, radii);
			GeometryBackgroundBorder geometry(out_mesh.vertices, out_mesh.indices);
			geometry.DrawBorder(metrics, band, colors);
		};
		ColourbPremultiplied outer_band[4], inner_band[4];
		for (int i = 0; i < 4; i++)
		{
			const BorderStyle style = BorderStyle(border_styles[i]);
			const bool top_left = (i == 0 || i == 3);
			outer_band[i] = inner_band[i] = transparent;
			if (style == BorderStyle::Groove)
			{
				outer_band[i] = (top_left ? DarkShade(border_colors[i]) : border_colors[i]);
				inner_band[i] = (top_left ? border_colors[i] : DarkShade(border_colors[i]));
			}
			else if (style == BorderStyle::Ridge)
			{
				outer_band[i] = (top_left ? border_colors[i] : DarkShade(border_colors[i]));
				inner_band[i] = (top_left ? DarkShade(border_colors[i]) : border_colors[i]);
			}
		}
		draw_band(0.f, 0.5f, outer_band);
		draw_band(0.5f, 1.f, inner_band);

		ColourbPremultiplied double_band[4];
		bool has_double = false;
		for (int i = 0; i < 4; i++)
		{
			const bool is_double = (BorderStyle(border_styles[i]) == BorderStyle::Double);
			double_band[i] = (is_double ? border_colors[i] : transparent);
			has_double |= is_double;
		}
		if (has_double)
		{
			// As in browsers, each line is a third of the width, rounded so thin borders keep visible lines.
			draw_band(0.f, 1.f / 3.f, double_band);
			draw_band(2.f / 3.f, 1.f, double_band);
		}
	}

	for (int i = 0; i < 4; i++)
	{
		const BorderStyle style = BorderStyle(border_styles[i]);
		if (style == BorderStyle::Dashed || style == BorderStyle::Dotted)
			GenerateDashedEdge(out_mesh, i, style == BorderStyle::Dotted, render_box, border_colors[i]);
	}
}

void MeshUtilities::GenerateBackground(Mesh& out_mesh, const RenderBox& render_box, ColourbPremultiplied color)
{
	const Vector2f fill_size = render_box.GetFillSize();
	const bool has_background = (color.alpha > 0 && fill_size.x > 0 && fill_size.y > 0);
	if (!has_background)
		return;

	const BorderMetrics metrics = GeometryBackgroundBorder::ComputeBorderMetrics(render_box.GetBorderOffset(), render_box.GetBorderWidths(),
		fill_size, render_box.GetBorderRadius());

	Vector<Vertex>& vertices = out_mesh.vertices;
	Vector<int>& indices = out_mesh.indices;

	// Reserve geometry. A conservative estimate, does not take border-radii into account.
	vertices.reserve((int)vertices.size() + 4);
	indices.reserve((int)indices.size() + 6);

	// Generate the geometry
	GeometryBackgroundBorder geometry(vertices, indices);
	geometry.DrawBackground(metrics, color);
}

} // namespace Rml
