#include "GridFormattingContext.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Context.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/ElementDocument.h"
#include "../../../Include/RmlUi/Core/ElementScroll.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "../../../Include/RmlUi/Core/Profiling.h"
#include "../../../Include/RmlUi/Core/StringUtilities.h"
#include "../CalcExpression.h"
#include "ContainerBox.h"
#include "LayoutDetails.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <float.h>

namespace Rml {

namespace {

	// -- Parsing --

	// Splits a value at top-level separators, keeping (...), [...] and quoted strings whole. Whitespace separates when 'separator' is ' '.
	StringList Split(const String& value, char separator)
	{
		StringList out;
		String current;
		int depth = 0;
		char quote = 0;
		for (char c : value)
		{
			if (quote)
			{
				current += c;
				if (c == quote)
					quote = 0;
				continue;
			}
			if (c == '"' || c == '\'')
				quote = c;
			else if (c == '(' || c == '[')
				depth++;
			else if ((c == ')' || c == ']') && depth > 0)
				depth--;

			const bool is_separator = (depth == 0 && (separator == ' ' ? (c == ' ' || c == '\t' || c == '\n' || c == '\r') : c == separator));
			if (is_separator)
			{
				if (separator != ' ' || !current.empty())
					out.push_back(StringUtilities::StripWhitespace(current));
				current.clear();
			}
			else
				current += c;
		}
		if (separator != ' ' || !StringUtilities::StripWhitespace(current).empty())
			out.push_back(StringUtilities::StripWhitespace(current));
		return out;
	}

	bool StartsWith(const String& value, const char* prefix)
	{
		return value.compare(0, strlen(prefix), prefix) == 0;
	}

	// The text between the first '(' and the matching last ')'.
	String FunctionArguments(const String& value)
	{
		const size_t open = value.find('(');
		const size_t close = value.rfind(')');
		if (open == String::npos || close == String::npos || close <= open)
			return String();
		return value.substr(open + 1, close - open - 1);
	}

	bool ParseInt(const String& value, int& out)
	{
		if (value.empty())
			return false;
		char* end = nullptr;
		const long result = strtol(value.c_str(), &end, 10);
		if (!end || *end != '\0')
			return false;
		out = (int)result;
		return true;
	}

	bool IsIdent(const String& value)
	{
		if (value.empty())
			return false;
		const char c = value[0];
		return (c == '_' || c == '-' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80) && value != "auto" &&
			value != "span";
	}

	struct Sizing {
		enum Kind { Length, Fr, Auto, MinContent, MaxContent, FitContent };
		Kind kind = Auto;
		float px = 0.f;      // Length and FitContent
		float percent = 0.f; // Length and FitContent: percentage of the grid's content size along the axis
		float fr = 0.f;

		bool IsIntrinsic() const { return kind == Auto || kind == MinContent || kind == MaxContent || kind == FitContent; }
		// The length, or -1 when it depends on an indefinite size.
		float Resolve(float base) const
		{
			if (percent != 0.f && base < 0.f)
				return -1.f;
			return px + (percent != 0.f ? percent * 0.01f * base : 0.f);
		}
	};

	struct TrackDef {
		Sizing min, max;
	};

	struct LengthResolver {
		Calc::Context context;

		bool Length(const String& token, float& px, float& percent) const
		{
			const String expression = (token.find('(') != String::npos ? token : "calc(" + token + ")");
			const Calc::Result result = Calc::Resolve(expression, context);
			if (!result.valid)
				return false;
			px = result.px + (result.number != 0.f && result.px == 0.f && result.percent == 0.f ? result.number : 0.f);
			percent = result.percent;
			return true;
		}

		bool Size(const String& token, Sizing& out) const
		{
			const String lower = StringUtilities::ToLower(token);
			out = Sizing{};
			if (lower == "auto")
				out.kind = Sizing::Auto;
			else if (lower == "min-content")
				out.kind = Sizing::MinContent;
			else if (lower == "max-content")
				out.kind = Sizing::MaxContent;
			else if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "fr") == 0 && lower.find('(') == String::npos)
			{
				out.kind = Sizing::Fr;
				out.fr = (float)atof(lower.c_str());
			}
			else if (StartsWith(lower, "fit-content("))
			{
				out.kind = Sizing::FitContent;
				return Length(FunctionArguments(lower), out.px, out.percent);
			}
			else
			{
				out.kind = Sizing::Length;
				return Length(lower, out.px, out.percent);
			}
			return true;
		}

		bool Track(const String& token, TrackDef& out) const
		{
			const String lower = StringUtilities::ToLower(token);
			if (StartsWith(lower, "minmax("))
			{
				const StringList args = Split(FunctionArguments(lower), ',');
				if (args.size() != 2 || !Size(args[0], out.min) || !Size(args[1], out.max))
					return false;
				if (out.min.kind == Sizing::Fr)
					out.min = Sizing{}; // A flexible minimum is invalid; treat as auto.
				return true;
			}
			Sizing size;
			if (!Size(lower, size))
				return false;
			out.max = size;
			out.min = (size.kind == Sizing::Fr || size.kind == Sizing::FitContent ? Sizing{} : size);
			return true;
		}
	};

	// A track list (grid-template-columns), with an optional auto-repeated part expanded once the available size is known.
	struct TrackList {
		struct Entry {
			bool is_names = false;
			StringList names;
			TrackDef track;
		};
		Vector<Entry> before, repeat, after;
		bool auto_fill = false, auto_fit = false;

		bool HasAutoRepeat() const { return auto_fill || auto_fit; }

		static void AddNames(Vector<Entry>& entries, const String& token)
		{
			Entry entry;
			entry.is_names = true;
			StringUtilities::ExpandString(entry.names, token.substr(1, token.size() - 2), ' ');
			entries.push_back(std::move(entry));
		}

		// Parses tokens into 'entries'. At the top level, an auto repeat switches the target to 'after'.
		bool ParseInto(Vector<Entry>* entries, const StringList& tokens, const LengthResolver& resolver, bool top_level)
		{
			for (const String& token : tokens)
			{
				if (token.empty())
					continue;
				if (token[0] == '[')
					AddNames(*entries, token);
				else if (StartsWith(StringUtilities::ToLower(token), "repeat("))
				{
					const String arguments = FunctionArguments(token);
					const size_t comma = arguments.find(',');
					if (comma == String::npos)
						return false;
					const String count_string = StringUtilities::ToLower(StringUtilities::StripWhitespace(arguments.substr(0, comma)));
					const StringList body = Split(arguments.substr(comma + 1), ' ');
					if (count_string == "auto-fill" || count_string == "auto-fit")
					{
						if (!top_level || HasAutoRepeat())
							return false;
						auto_fill = (count_string == "auto-fill");
						auto_fit = !auto_fill;
						if (!ParseInto(&repeat, body, resolver, false))
							return false;
						entries = &after;
						continue;
					}
					int count = 0;
					if (!ParseInt(count_string, count) || count < 1)
						return false;
					count = Math::Min(count, 1000);
					for (int i = 0; i < count; i++)
						if (!ParseInto(entries, body, resolver, false))
							return false;
				}
				else
				{
					Entry entry;
					if (!resolver.Track(token, entry.track))
						return false;
					entries->push_back(std::move(entry));
				}
			}
			return true;
		}

		bool Parse(const String& value, const LengthResolver& resolver)
		{
			const String lower = StringUtilities::ToLower(StringUtilities::StripWhitespace(value));
			if (lower.empty() || lower == "none")
				return true;
			return ParseInto(&before, Split(value, ' '), resolver, true);
		}

		// Expands the list with the given number of repetitions into tracks and the names of each line (tracks + 1 lines).
		void Expand(int repetitions, Vector<TrackDef>& tracks, Vector<StringList>& line_names, int& repeat_begin, int& repeat_end) const
		{
			tracks.clear();
			line_names.assign(1, StringList());
			auto append = [&](const Vector<Entry>& entries) {
				for (const Entry& entry : entries)
				{
					if (entry.is_names)
						line_names.back().insert(line_names.back().end(), entry.names.begin(), entry.names.end());
					else
					{
						tracks.push_back(entry.track);
						line_names.emplace_back();
					}
				}
			};
			append(before);
			repeat_begin = (int)tracks.size();
			for (int i = 0; i < repetitions; i++)
				append(repeat);
			repeat_end = (int)tracks.size();
			append(after);
		}
	};

	struct Area {
		int row_start = INT_MAX, row_end = -1, column_start = INT_MAX, column_end = -1;
	};

	// Reads grid-template-areas strings ("head head" "side main") into named rectangles. Returns the number of rows and columns.
	void ParseAreas(const String& value, UnorderedMap<String, Area>& areas, int& num_rows, int& num_columns)
	{
		num_rows = num_columns = 0;
		size_t i = 0;
		while (i < value.size())
		{
			const char quote = value[i];
			if (quote != '"' && quote != '\'')
			{
				i++;
				continue;
			}
			const size_t close = value.find(quote, i + 1);
			if (close == String::npos)
				break;
			StringList cells;
			StringUtilities::ExpandString(cells, value.substr(i + 1, close - i - 1), ' ');
			int column = 0;
			for (const String& cell : cells)
			{
				if (cell.empty())
					continue;
				if (cell.find_first_not_of('.') != String::npos)
				{
					Area& area = areas[cell];
					area.row_start = Math::Min(area.row_start, num_rows);
					area.row_end = Math::Max(area.row_end, num_rows + 1);
					area.column_start = Math::Min(area.column_start, column);
					area.column_end = Math::Max(area.column_end, column + 1);
				}
				column++;
			}
			num_columns = Math::Max(num_columns, column);
			num_rows++;
			i = close + 1;
		}
	}

	// -- Placement --

	struct LinePlacement {
		bool is_auto = true;
		int span = 0; // 0 when not a span
		int number = 0;
		String name;
	};

	LinePlacement ParseLine(const String& value)
	{
		LinePlacement line;
		const StringList tokens = Split(StringUtilities::StripWhitespace(value), ' ');
		bool span = false;
		for (const String& token : tokens)
		{
			const String lower = StringUtilities::ToLower(token);
			int number = 0;
			if (lower == "span")
				span = true;
			else if (lower == "auto" || lower.empty())
				continue;
			else if (ParseInt(lower, number))
				line.number = number;
			else if (IsIdent(token))
				line.name = token;
		}
		if (span)
		{
			line.span = Math::Max(line.number, 1);
			line.number = 0;
			line.is_auto = false;
		}
		else
			line.is_auto = (line.number == 0 && line.name.empty());
		return line;
	}

	struct AxisLines {
		int explicit_tracks = 0;
		const Vector<StringList>* line_names = nullptr;
		const UnorderedMap<String, Area>* areas = nullptr;
		bool rows = false;

		// Line index (0-based) for a name, or -1.
		int Named(const String& name, int nth, bool is_start) const
		{
			Vector<int> lines;
			if (line_names)
				for (int i = 0; i < (int)line_names->size(); i++)
					if (std::find((*line_names)[i].begin(), (*line_names)[i].end(), name) != (*line_names)[i].end())
						lines.push_back(i);
			if (areas)
			{
				auto add_area_line = [&](const String& area_name, bool start) {
					auto it = areas->find(area_name);
					if (it != areas->end())
					{
						const Area& area = it->second;
						lines.push_back(rows ? (start ? area.row_start : area.row_end) : (start ? area.column_start : area.column_end));
					}
				};
				if (name.size() > 6 && name.compare(name.size() - 6, 6, "-start") == 0)
					add_area_line(name.substr(0, name.size() - 6), true);
				else if (name.size() > 4 && name.compare(name.size() - 4, 4, "-end") == 0)
					add_area_line(name.substr(0, name.size() - 4), false);
				else if (lines.empty())
					add_area_line(name, is_start);
			}
			if (lines.empty())
				return -1;
			std::sort(lines.begin(), lines.end());
			if (nth == 0)
				nth = 1;
			if (nth > 0)
				return lines[Math::Min(nth, (int)lines.size()) - 1];
			return lines[Math::Max(0, (int)lines.size() + nth)];
		}

		// Line index (0-based) of a definite line placement, or -1.
		int Resolve(const LinePlacement& line, bool is_start) const
		{
			if (line.is_auto || line.span > 0)
				return -1;
			if (!line.name.empty())
				return Named(line.name, line.number, is_start);
			if (line.number > 0)
				return line.number - 1;
			return Math::Max(0, explicit_tracks + 1 + line.number);
		}
	};

	struct Placement {
		int start = -1; // -1 for auto
		int span = 1;
	};

	Placement ResolvePlacement(const LinePlacement& start, const LinePlacement& end, const AxisLines& axis)
	{
		Placement placement;
		const int start_line = axis.Resolve(start, true);
		const int end_line = axis.Resolve(end, false);
		if (start_line >= 0 && end_line >= 0)
		{
			placement.start = Math::Min(start_line, end_line);
			placement.span = Math::Max(1, std::abs(end_line - start_line));
		}
		else if (start_line >= 0)
		{
			placement.start = start_line;
			placement.span = Math::Max(1, end.span);
		}
		else if (end_line >= 0)
		{
			placement.span = Math::Max(1, start.span);
			placement.start = Math::Max(0, end_line - placement.span);
		}
		else
			placement.span = Math::Max(1, Math::Max(start.span, end.span));
		placement.span = Math::Min(placement.span, 1000);
		return placement;
	}

	// -- Track sizing --

	struct Track {
		TrackDef def;
		float base = 0.f;
		float limit = -1.f; // -1 while not set by any item
		float size = 0.f;
		bool collapsed = false; // auto-fit track without items
	};

	struct AxisItem {
		int start = 0, span = 1;
		float min_contribution = 0.f, max_contribution = 0.f; // Outer sizes
	};

	// Sizes the tracks of one axis (CSS Grid §11, simplified). 'available' is negative when indefinite.
	void SizeTracks(Vector<Track>& tracks, const Vector<AxisItem>& items, float available, float gap, bool stretch_auto)
	{
		const int num_tracks = (int)tracks.size();
		if (num_tracks == 0)
			return;
		int num_gaps = -1;
		for (const Track& track : tracks)
			num_gaps += (track.collapsed ? 0 : 1);
		const float total_gaps = gap * Math::Max(0, num_gaps);

		auto is_flex = [](const Track& track) { return track.def.max.kind == Sizing::Fr; };

		// Initialize base sizes and growth limits.
		for (Track& track : tracks)
		{
			track.base = 0.f;
			track.limit = -1.f;
			if (track.collapsed)
				continue;
			if (track.def.min.kind == Sizing::Length)
			{
				const float length = track.def.min.Resolve(available);
				track.base = Math::Max(0.f, length);
				if (length < 0.f)
					track.def.min = Sizing{}; // Percentage of an indefinite size: treat as auto.
			}
			if (track.def.max.kind == Sizing::Length)
			{
				const float length = track.def.max.Resolve(available);
				if (length < 0.f)
					track.def.max = Sizing{};
				else
					track.limit = Math::Max(length, track.base);
			}
		}

		auto span_has_flex = [&](const AxisItem& item) {
			for (int i = item.start; i < item.start + item.span && i < num_tracks; i++)
				if (is_flex(tracks[i]))
					return true;
			return false;
		};
		auto gaps_within = [&](const AxisItem& item) {
			int count = -1;
			for (int i = item.start; i < item.start + item.span && i < num_tracks; i++)
				count += (tracks[i].collapsed ? 0 : 1);
			return gap * Math::Max(0, count);
		};

		// Items spanning a single non-flexible track.
		for (const AxisItem& item : items)
		{
			if (item.span != 1 || item.start >= num_tracks)
				continue;
			Track& track = tracks[item.start];
			if (track.collapsed)
				continue;
			const Sizing& min = track.def.min;
			const Sizing& max = track.def.max;
			if (min.kind == Sizing::MaxContent)
				track.base = Math::Max(track.base, item.max_contribution);
			else if (min.IsIntrinsic())
				track.base = Math::Max(track.base, item.min_contribution);

			if (max.kind == Sizing::MinContent)
				track.limit = Math::Max(track.limit, item.min_contribution);
			else if (max.kind == Sizing::Auto || max.kind == Sizing::MaxContent)
				track.limit = Math::Max(track.limit, item.max_contribution);
			else if (max.kind == Sizing::FitContent)
			{
				float fit = max.Resolve(available);
				if (fit < 0.f)
					fit = FLT_MAX;
				track.limit = Math::Max(track.limit, Math::Min(item.max_contribution, Math::Max(item.min_contribution, fit)));
			}
		}

		// Items spanning several tracks: distribute what does not fit equally over the spanned intrinsic tracks.
		for (const AxisItem& item : items)
		{
			if (item.span < 2 || item.start >= num_tracks)
				continue;
			const bool flex = span_has_flex(item);
			const int end = Math::Min(item.start + item.span, num_tracks);
			float sum_base = gaps_within(item), sum_limit = gaps_within(item);
			Vector<int> intrinsic_min, intrinsic_max;
			for (int i = item.start; i < end; i++)
			{
				const Track& track = tracks[i];
				if (track.collapsed)
					continue;
				sum_base += track.base;
				sum_limit += (track.limit >= 0.f ? track.limit : track.base);
				if (flex ? (is_flex(track) && track.def.min.IsIntrinsic()) : track.def.min.IsIntrinsic())
					intrinsic_min.push_back(i);
				if (!flex && track.def.max.IsIntrinsic())
					intrinsic_max.push_back(i);
			}
			const float extra_base = item.min_contribution - sum_base;
			if (extra_base > 0.f && !intrinsic_min.empty())
				for (int i : intrinsic_min)
					tracks[i].base += extra_base / float(intrinsic_min.size());
			const float extra_limit = item.max_contribution - sum_limit;
			if (extra_limit > 0.f && !intrinsic_max.empty())
				for (int i : intrinsic_max)
					tracks[i].limit = (tracks[i].limit >= 0.f ? tracks[i].limit : tracks[i].base) + extra_limit / float(intrinsic_max.size());
		}

		for (Track& track : tracks)
		{
			if (track.limit < 0.f)
				track.limit = (track.def.max.kind == Sizing::Auto && !track.collapsed ? track.base : track.base);
			track.limit = Math::Max(track.limit, track.base);
			track.size = track.base;
		}

		// Maximize the non-flexible tracks toward their growth limits.
		if (available >= 0.f)
		{
			float free_space = available - total_gaps;
			for (const Track& track : tracks)
				free_space -= track.size;
			for (int iteration = 0; iteration < num_tracks && free_space > 0.01f; iteration++)
			{
				int growable = 0;
				for (const Track& track : tracks)
					if (!is_flex(track) && !track.collapsed && track.size < track.limit)
						growable++;
				if (growable == 0)
					break;
				const float share = free_space / float(growable);
				for (Track& track : tracks)
				{
					if (is_flex(track) || track.collapsed || track.size >= track.limit)
						continue;
					const float grow = Math::Min(share, track.limit - track.size);
					track.size += grow;
					free_space -= grow;
				}
			}
		}
		else
		{
			for (Track& track : tracks)
				if (!is_flex(track))
					track.size = track.limit;
		}

		// Flexible tracks.
		bool has_flex = false;
		for (const Track& track : tracks)
			has_flex |= (is_flex(track) && !track.collapsed);
		if (has_flex)
		{
			float fr_size = 0.f;
			if (available >= 0.f)
			{
				Vector<bool> inflexible(num_tracks, false);
				for (int iteration = 0; iteration <= num_tracks; iteration++)
				{
					float leftover = available - total_gaps;
					float sum_factors = 0.f;
					for (int i = 0; i < num_tracks; i++)
					{
						const Track& track = tracks[i];
						if (track.collapsed)
							continue;
						if (is_flex(track) && !inflexible[i])
							sum_factors += track.def.max.fr;
						else
							leftover -= (is_flex(track) ? track.base : track.size);
					}
					fr_size = Math::Max(0.f, leftover) / Math::Max(sum_factors, 1.f);
					bool changed = false;
					for (int i = 0; i < num_tracks; i++)
					{
						const Track& track = tracks[i];
						if (is_flex(track) && !track.collapsed && !inflexible[i] && track.base > fr_size * track.def.max.fr)
						{
							inflexible[i] = true;
							changed = true;
						}
					}
					if (!changed)
						break;
				}
			}
			else
			{
				for (const Track& track : tracks)
					if (is_flex(track) && !track.collapsed)
						fr_size = Math::Max(fr_size, track.def.max.fr > 1.f ? track.base / track.def.max.fr : track.base);
				for (const AxisItem& item : items)
				{
					if (!span_has_flex(item))
						continue;
					float fixed = gaps_within(item), sum_factors = 0.f;
					for (int i = item.start; i < Math::Min(item.start + item.span, num_tracks); i++)
					{
						if (is_flex(tracks[i]))
							sum_factors += tracks[i].def.max.fr;
						else
							fixed += tracks[i].size;
					}
					if (sum_factors > 0.f)
						fr_size = Math::Max(fr_size, (item.max_contribution - fixed) / Math::Max(sum_factors, 1.f));
				}
			}
			for (Track& track : tracks)
				if (is_flex(track) && !track.collapsed)
					track.size = Math::Max(track.base, fr_size * track.def.max.fr);
		}

		// Stretch auto tracks into what is left.
		if (available >= 0.f && stretch_auto)
		{
			float free_space = available - total_gaps;
			int num_auto = 0;
			for (const Track& track : tracks)
			{
				free_space -= track.size;
				num_auto += (track.def.max.kind == Sizing::Auto && !track.collapsed ? 1 : 0);
			}
			if (free_space > 0.f && num_auto > 0)
				for (Track& track : tracks)
					if (track.def.max.kind == Sizing::Auto && !track.collapsed)
						track.size += free_space / float(num_auto);
		}

		for (Track& track : tracks)
			if (track.collapsed)
				track.size = 0.f;
	}

	// Positions the tracks along an axis with content distribution. Returns the start offset of each track.
	Vector<float> PositionTracks(const Vector<Track>& tracks, float gap, float available, Style::JustifyContent distribution, float& used_size)
	{
		using Style::JustifyContent;
		const int num_tracks = (int)tracks.size();
		Vector<float> positions(num_tracks + 1, 0.f);
		float sum = 0.f;
		int visible = 0;
		for (const Track& track : tracks)
		{
			sum += track.size;
			visible += (track.collapsed ? 0 : 1);
		}
		sum += gap * Math::Max(0, visible - 1);

		float offset = 0.f, between = 0.f;
		const float free_space = (available >= 0.f ? available - sum : 0.f);
		if (free_space > 0.f && visible > 0)
		{
			switch (distribution)
			{
			case JustifyContent::FlexEnd: offset = free_space; break;
			case JustifyContent::Center: offset = 0.5f * free_space; break;
			case JustifyContent::SpaceBetween: between = (visible > 1 ? free_space / float(visible - 1) : 0.f); break;
			case JustifyContent::SpaceAround:
				between = free_space / float(visible);
				offset = 0.5f * between;
				break;
			case JustifyContent::SpaceEvenly:
				between = free_space / float(visible + 1);
				offset = between;
				break;
			default: break;
			}
		}

		float cursor = offset;
		bool first = true;
		for (int i = 0; i < num_tracks; i++)
		{
			if (!tracks[i].collapsed)
			{
				if (!first)
					cursor += gap + between;
				first = false;
			}
			positions[i] = cursor;
			cursor += tracks[i].size;
		}
		positions[num_tracks] = cursor;
		used_size = sum;
		return positions;
	}

	struct GridItem {
		Element* element = nullptr;
		Placement column, row;
		Box box;
		float edges_x = 0.f, edges_y = 0.f; // Margin + border + padding
		float min_content = -1.f, max_content = -1.f;
		float height = 0.f; // Outer height for the used width
		float used_width = 0.f;
	};

	String GetString(Element* element, const char* name)
	{
		const Property* property = element->GetProperty(name);
		return (property && property->unit == Unit::STRING ? StringUtilities::StripWhitespace(property->Get<String>()) : String());
	}

	int GetKeyword(Element* element, const char* name, int default_value)
	{
		const Property* property = element->GetProperty(name);
		return (property && property->unit == Unit::KEYWORD ? property->Get<int>() : default_value);
	}

	enum class SelfAlign { Normal, Start, End, Center, Stretch };

	SelfAlign FromAlignSelf(Style::AlignSelf align)
	{
		switch (align)
		{
		case Style::AlignSelf::FlexStart:
		case Style::AlignSelf::Baseline: return SelfAlign::Start;
		case Style::AlignSelf::FlexEnd: return SelfAlign::End;
		case Style::AlignSelf::Center: return SelfAlign::Center;
		case Style::AlignSelf::Stretch: return SelfAlign::Stretch;
		case Style::AlignSelf::Auto: break;
		}
		return SelfAlign::Normal;
	}

} // namespace

UniquePtr<LayoutBox> GridFormattingContext::Format(ContainerBox* parent_container, Element* element, const Box* override_initial_box)
{
	RMLUI_ZoneScopedC(0xAF4FAF);
	auto container_box = MakeUnique<FlexContainer>(element, parent_container);

	ElementScroll* element_scroll = element->GetElementScroll();
	const ComputedValues& computed = element->GetComputedValues();

	const Vector2f containing_block = LayoutDetails::GetContainingBlock(parent_container, element->GetPosition()).size;

	Box& box = container_box->GetBox();
	if (override_initial_box)
		box = *override_initial_box;
	else
		LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::Block);

	container_box->ResetScrollbars(box);

	GridFormattingContext context;
	context.container_box = container_box.get();
	context.element_grid = element;

	float min_height = 0.f, max_height = FLT_MAX;
	LayoutDetails::GetMinMaxHeight(min_height, max_height, computed, box, containing_block.y);

	const Vector2f box_content_size = box.GetSize();
	const bool auto_height = (box_content_size.y < 0.0f);
	context.content_offset = box.GetPosition();

	for (int layout_iteration = 0; layout_iteration < 3; layout_iteration++)
	{
		const Vector2f scrollbar_size = {
			element_scroll->GetScrollbarSize(ElementScroll::VERTICAL),
			element_scroll->GetScrollbarSize(ElementScroll::HORIZONTAL),
		};

		context.available_content_size = Math::Max(box_content_size - scrollbar_size, Vector2f(0.f));
		context.content_containing_block = context.available_content_size;
		if (auto_height)
		{
			context.available_content_size.y = -1.f;
			context.content_containing_block.y = containing_block.y;
		}

		Vector2f resulting_content_size, visible_overflow_size, scrollable_content_size;
		float baseline = 0.f;
		context.Format(resulting_content_size, visible_overflow_size, scrollable_content_size, baseline);

		if (auto_height && !container_box->CatchOverflow(visible_overflow_size, scrollable_content_size, box, max_height))
			continue;

		Vector2f formatted_content_size = box_content_size;
		if (auto_height)
			formatted_content_size.y = Math::Clamp(resulting_content_size.y, min_height, max_height) + scrollbar_size.y;

		Box sized_box = box;
		sized_box.SetContent(formatted_content_size);

		const float element_baseline =
			sized_box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border) + sized_box.GetEdge(BoxArea::Margin, BoxEdge::Bottom) - baseline;

		if (container_box->Close(visible_overflow_size, scrollable_content_size, sized_box, element_baseline))
			break;
	}

	return container_box;
}

Vector2f GridFormattingContext::GetMaxContentSize(Element* element)
{
	const Vector2f infinity(10000.0f, 10000.0f);
	RootBox root(infinity);
	auto container_box = MakeUnique<FlexContainer>(element, &root);

	GridFormattingContext context;
	context.container_box = container_box.get();
	context.element_grid = element;
	context.available_content_size = Vector2f(-1, -1);
	context.content_containing_block = infinity;

	Vector2f resulting_content_size, visible_overflow_size, scrollable_content_size;
	float baseline = 0.f;
	context.Format(resulting_content_size, visible_overflow_size, scrollable_content_size, baseline);
	return resulting_content_size;
}

void GridFormattingContext::Format(Vector2f& resulting_content_size, Vector2f& visible_overflow_size, Vector2f& scrollable_content_size,
	float& baseline) const
{
	const ComputedValues& computed = element_grid->GetComputedValues();

	LengthResolver resolver;
	resolver.context.font_size = computed.font_size();
	if (ElementDocument* document = element_grid->GetOwnerDocument())
		resolver.context.root_font_size = document->GetComputedValues().font_size();
	if (Context* ui_context = element_grid->GetContext())
	{
		resolver.context.dp_ratio = ui_context->GetDensityIndependentPixelRatio();
		resolver.context.viewport = Vector2f(ui_context->GetDimensions());
	}

	// -- Read the template --
	String template_columns = GetString(element_grid, "grid-template-columns");
	String template_rows = GetString(element_grid, "grid-template-rows");
	String template_areas = GetString(element_grid, "grid-template-areas");
	String auto_flow = StringUtilities::ToLower(GetString(element_grid, "grid-auto-flow"));
	String auto_rows = GetString(element_grid, "grid-auto-rows");
	String auto_columns = GetString(element_grid, "grid-auto-columns");

	// The grid-template and grid shorthands fill in what the longhands leave unset.
	for (const char* shorthand : {"grid-template", "grid"})
	{
		const String value = GetString(element_grid, shorthand);
		if (value.empty() || StringUtilities::ToLower(value) == "none")
			continue;
		const StringList parts = Split(value, '/');
		String rows_part = (parts.size() >= 1 ? parts[0] : String());
		String columns_part = (parts.size() >= 2 ? parts[1] : String());
		auto take_auto_flow = [&](String& part, String& auto_tracks, const char* flow) {
			const String lower = StringUtilities::ToLower(part);
			if (lower.find("auto-flow") == String::npos)
				return false;
			String tracks;
			for (const String& token : Split(part, ' '))
			{
				const String lower_token = StringUtilities::ToLower(token);
				if (lower_token == "auto-flow")
					auto_flow = flow;
				else if (lower_token == "dense")
					auto_flow += " dense";
				else
					tracks += token + " ";
			}
			auto_tracks = tracks;
			part.clear();
			return true;
		};
		take_auto_flow(rows_part, auto_rows, "row");
		take_auto_flow(columns_part, auto_columns, "column");

		if (template_columns.empty())
			template_columns = columns_part;
		if (rows_part.find('"') != String::npos || rows_part.find('\'') != String::npos)
		{
			// "a a" 40px "b b" 1fr: area rows, each optionally followed by its size.
			String areas, rows;
			bool row_has_size = true;
			for (const String& token : Split(rows_part, ' '))
			{
				if (token.empty())
					continue;
				if (token[0] == '"' || token[0] == '\'')
				{
					if (!row_has_size)
						rows += "auto ";
					areas += token + " ";
					row_has_size = false;
				}
				else if (token[0] == '[')
					rows += token + " ";
				else
				{
					rows += token + " ";
					row_has_size = true;
				}
			}
			if (!row_has_size)
				rows += "auto";
			if (template_areas.empty())
				template_areas = areas;
			if (template_rows.empty())
				template_rows = rows;
		}
		else if (template_rows.empty())
			template_rows = rows_part;
	}

	const bool flow_column = (auto_flow.find("column") != String::npos);
	const bool dense = (auto_flow.find("dense") != String::npos);

	TrackList column_list, row_list, auto_column_list, auto_row_list;
	if (!column_list.Parse(template_columns, resolver))
		column_list = TrackList{};
	if (!row_list.Parse(template_rows, resolver))
		row_list = TrackList{};
	if (!auto_column_list.Parse(auto_columns, resolver) || auto_column_list.HasAutoRepeat())
		auto_column_list = TrackList{};
	if (!auto_row_list.Parse(auto_rows, resolver) || auto_row_list.HasAutoRepeat())
		auto_row_list = TrackList{};

	UnorderedMap<String, Area> areas;
	int area_rows = 0, area_columns = 0;
	ParseAreas(template_areas, areas, area_rows, area_columns);

	const float column_gap = Math::Max(0.f, ResolveValue(computed.column_gap(), Math::Max(available_content_size.x, 0.f)));
	const float row_gap = Math::Max(0.f, ResolveValue(computed.row_gap(), Math::Max(available_content_size.y, 0.f)));

	// Number of auto repetitions: as many as fit in the available size, at least one.
	auto repetitions = [](const TrackList& list, float available, float gap) {
		if (!list.HasAutoRepeat())
			return 0;
		if (available < 0.f)
			return 1;
		float repeat_size = 0.f, other_size = 0.f;
		int repeat_count = 0, other_count = 0;
		auto fixed_size = [&](const TrackDef& def) {
			float size = (def.max.kind == Sizing::Length ? def.max.Resolve(available) : -1.f);
			if (size < 0.f && def.min.kind == Sizing::Length)
				size = def.min.Resolve(available);
			return size;
		};
		for (const auto& entry : list.repeat)
			if (!entry.is_names)
			{
				const float size = fixed_size(entry.track);
				if (size <= 0.f)
					return 1;
				repeat_size += size;
				repeat_count++;
			}
		for (const Vector<TrackList::Entry>* entries : {&list.before, &list.after})
			for (const auto& entry : *entries)
				if (!entry.is_names)
				{
					other_size += Math::Max(0.f, fixed_size(entry.track));
					other_count++;
				}
		if (repeat_count == 0)
			return 1;
		const float room = available - other_size - gap * float(other_count - 1);
		const int count = (int)Math::Max(1.f, std::floor(room / (repeat_size + gap * float(repeat_count))));
		return Math::Min(count, 1000);
	};

	Vector<TrackDef> column_defs, row_defs;
	Vector<StringList> column_names, row_names;
	int column_repeat_begin = 0, column_repeat_end = 0, row_repeat_begin = 0, row_repeat_end = 0;
	column_list.Expand(repetitions(column_list, available_content_size.x, column_gap), column_defs, column_names, column_repeat_begin,
		column_repeat_end);
	row_list.Expand(repetitions(row_list, available_content_size.y, row_gap), row_defs, row_names, row_repeat_begin, row_repeat_end);

	const int explicit_columns = Math::Max((int)column_defs.size(), area_columns);
	const int explicit_rows = Math::Max((int)row_defs.size(), area_rows);

	AxisLines column_lines{explicit_columns, &column_names, &areas, false};
	AxisLines row_lines{explicit_rows, &row_names, &areas, true};

	// -- Collect items --
	Vector<GridItem> items;
	const int num_children = element_grid->GetNumChildren();
	items.reserve(num_children);
	for (int i = 0; i < num_children; i++)
	{
		Element* element = element_grid->GetChild(i);
		const ComputedValues& item_computed = element->GetComputedValues();
		if (item_computed.display() == Style::Display::None)
			continue;
		if (auto text = rmlui_dynamic_cast<ElementText*>(element))
		{
			if (StringUtilities::StripWhitespace(text->GetText()).empty())
				continue;
		}
		if (item_computed.position() == Style::Position::Absolute || item_computed.position() == Style::Position::Fixed)
		{
			ContainerBox* absolute_containing_block = LayoutDetails::GetContainingBlock(container_box, item_computed.position()).container;
			absolute_containing_block->AddAbsoluteElement(element, {}, element_grid);
			continue;
		}
		if (item_computed.position() == Style::Position::Relative)
			container_box->AddRelativeElement(element);

		GridItem item;
		item.element = element;

		LinePlacement row_start, row_end, column_start, column_end;
		const String area = GetString(element, "grid-area");
		if (!area.empty())
		{
			const StringList parts = Split(area, '/');
			auto part = [&](size_t index) { return index < parts.size() ? ParseLine(parts[index]) : LinePlacement{}; };
			row_start = part(0);
			column_start = parts.size() >= 2 ? part(1) : (IsIdent(row_start.name) && row_start.number == 0 ? row_start : LinePlacement{});
			row_end = parts.size() >= 3 ? part(2) : (IsIdent(row_start.name) && row_start.number == 0 ? row_start : LinePlacement{});
			column_end = parts.size() >= 4 ? part(3) : (IsIdent(column_start.name) && column_start.number == 0 ? column_start : LinePlacement{});
		}
		for (int axis = 0; axis < 2; axis++)
		{
			LinePlacement& start = (axis == 0 ? row_start : column_start);
			LinePlacement& end = (axis == 0 ? row_end : column_end);
			const String shorthand = GetString(element, axis == 0 ? "grid-row" : "grid-column");
			if (!shorthand.empty())
			{
				const StringList parts = Split(shorthand, '/');
				start = ParseLine(parts[0]);
				end = (parts.size() >= 2 ? ParseLine(parts[1]) : (IsIdent(start.name) && start.number == 0 ? start : LinePlacement{}));
			}
			const String start_value = GetString(element, axis == 0 ? "grid-row-start" : "grid-column-start");
			const String end_value = GetString(element, axis == 0 ? "grid-row-end" : "grid-column-end");
			if (!start_value.empty())
				start = ParseLine(start_value);
			if (!end_value.empty())
				end = ParseLine(end_value);
		}
		item.row = ResolvePlacement(row_start, row_end, row_lines);
		item.column = ResolvePlacement(column_start, column_end, column_lines);
		items.push_back(std::move(item));
	}

	// -- Place items (§8.5) --
	// 'major' is the axis the auto-placement cursor advances along line by line (rows for row flow), 'minor' is the axis it fills.
	auto major = [flow_column](GridItem& item) -> Placement& { return flow_column ? item.column : item.row; };
	auto minor = [flow_column](GridItem& item) -> Placement& { return flow_column ? item.row : item.column; };

	int minor_count = (flow_column ? explicit_rows : explicit_columns);
	int major_count = (flow_column ? explicit_columns : explicit_rows);
	for (GridItem& item : items)
	{
		const Placement& m = minor(item);
		minor_count = Math::Max(minor_count, (m.start >= 0 ? m.start : 0) + m.span);
	}
	minor_count = Math::Max(minor_count, 1);

	Vector<Vector<bool>> occupied; // [major][minor]
	auto ensure_major = [&](int count) {
		while ((int)occupied.size() < count)
			occupied.emplace_back(minor_count, false);
	};
	auto fits = [&](int major_start, int major_span, int minor_start, int minor_span) {
		if (minor_start < 0 || minor_start + minor_span > minor_count)
			return false;
		ensure_major(major_start + major_span);
		for (int a = major_start; a < major_start + major_span; a++)
			for (int b = minor_start; b < minor_start + minor_span; b++)
				if (occupied[a][b])
					return false;
		return true;
	};
	auto occupy = [&](GridItem& item) {
		const Placement& a = major(item);
		const Placement& b = minor(item);
		ensure_major(a.start + a.span);
		for (int i = a.start; i < a.start + a.span; i++)
			for (int j = b.start; j < b.start + b.span; j++)
				occupied[i][j] = true;
	};

	// 1. Items with both positions definite.
	for (GridItem& item : items)
		if (item.row.start >= 0 && item.column.start >= 0)
			occupy(item);

	// 2. Items locked to a major line.
	{
		Vector<int> cursors;
		for (GridItem& item : items)
		{
			Placement& a = major(item);
			Placement& b = minor(item);
			if (a.start < 0 || b.start >= 0)
				continue;
			if ((int)cursors.size() <= a.start)
				cursors.resize(a.start + 1, 0);
			int position = (dense ? 0 : cursors[a.start]);
			while (!fits(a.start, a.span, position, b.span) && position + b.span <= minor_count)
				position++;
			if (position + b.span > minor_count)
				position = 0; // Does not fit: overlap at the start.
			b.start = position;
			cursors[a.start] = position + b.span;
			occupy(item);
		}
	}

	// 3. Auto-placed items.
	{
		int cursor_major = 0, cursor_minor = 0;
		for (GridItem& item : items)
		{
			Placement& a = major(item);
			Placement& b = minor(item);
			if (a.start >= 0)
				continue;
			if (dense)
				cursor_major = cursor_minor = 0;

			if (b.start >= 0)
			{
				if (b.start < cursor_minor)
					cursor_major++;
				cursor_minor = b.start;
				while (!fits(cursor_major, a.span, b.start, b.span))
					cursor_major++;
				a.start = cursor_major;
			}
			else
			{
				for (;;)
				{
					if (cursor_minor + b.span > minor_count)
					{
						cursor_major++;
						cursor_minor = 0;
					}
					if (fits(cursor_major, a.span, cursor_minor, b.span))
						break;
					cursor_minor++;
				}
				a.start = cursor_major;
				b.start = cursor_minor;
				cursor_minor += b.span;
			}
			occupy(item);
		}
	}

	for (GridItem& item : items)
		major_count = Math::Max(major_count, major(item).start + major(item).span);
	const int num_columns = Math::Max(flow_column ? major_count : minor_count, explicit_columns);
	const int num_rows = Math::Max(flow_column ? minor_count : major_count, explicit_rows);

	// -- Build the tracks, explicit ones from the template and implicit ones from grid-auto-rows/columns --
	auto build_tracks = [](const Vector<TrackDef>& defs, const TrackList& auto_list, int count, const LengthResolver&) {
		Vector<TrackDef> auto_defs;
		Vector<StringList> unused_names;
		int b = 0, e = 0;
		auto_list.Expand(0, auto_defs, unused_names, b, e);
		Vector<Track> tracks(count);
		for (int i = 0; i < count; i++)
		{
			if (i < (int)defs.size())
				tracks[i].def = defs[i];
			else if (!auto_defs.empty())
				tracks[i].def = auto_defs[(i - (int)defs.size()) % auto_defs.size()];
		}
		return tracks;
	};
	Vector<Track> columns = build_tracks(column_defs, auto_column_list, num_columns, resolver);
	Vector<Track> rows = build_tracks(row_defs, auto_row_list, num_rows, resolver);

	// auto-fit: repeated tracks without items collapse.
	auto collapse_empty = [&](Vector<Track>& tracks, bool is_columns, int begin, int end) {
		for (int i = begin; i < end && i < (int)tracks.size(); i++)
		{
			bool used = false;
			for (const GridItem& item : items)
			{
				const Placement& p = (is_columns ? item.column : item.row);
				used |= (i >= p.start && i < p.start + p.span);
			}
			tracks[i].collapsed = !used;
		}
	};
	if (column_list.auto_fit)
		collapse_empty(columns, true, column_repeat_begin, column_repeat_end);
	if (row_list.auto_fit)
		collapse_empty(rows, false, row_repeat_begin, row_repeat_end);

	// -- Column sizes --
	const Vector2f item_containing_block = {Math::Max(content_containing_block.x, 0.f), content_containing_block.y};
	for (GridItem& item : items)
	{
		LayoutDetails::BuildBox(item.box, item_containing_block, item.element, BuildBoxMode::UnalignedBlock);
		item.edges_x = item.box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Margin, BoxArea::Padding);
		item.edges_y = item.box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Margin, BoxArea::Padding);
	}

	auto content_widths = [&](GridItem& item) {
		if (item.max_content >= 0.f)
			return;
		if (item.box.GetSize().x >= 0.f)
		{
			item.min_content = item.max_content = item.box.GetSize().x + item.edges_x;
			return;
		}
		const Vector2f unconstrained(100000.f, item_containing_block.y);
		item.max_content = LayoutDetails::GetShrinkToFitWidth(item.element, unconstrained) + item.edges_x;

		// Min-content: format under a zero width and measure what overflows (the longest word or widest child).
		Box zero_box = item.box;
		zero_box.SetContent(Vector2f(0.f, item.box.GetSize().y));
		RootBox root(Vector2f(0.f, item_containing_block.y));
		UniquePtr<LayoutBox> layout_box = FormattingContext::FormatIndependent(&root, item.element, &zero_box, FormattingContextType::Block);
		const float border_box_width = (layout_box ? layout_box->GetVisibleOverflowSize().x : 0.f);
		const float margins = item.box.GetEdge(BoxArea::Margin, BoxEdge::Left) + item.box.GetEdge(BoxArea::Margin, BoxEdge::Right);
		item.min_content = Math::Min(border_box_width + margins, item.max_content);

		float min_width = 0.f, max_width = FLT_MAX;
		LayoutDetails::GetMinMaxWidth(min_width, max_width, item.element->GetComputedValues(), item.box, item_containing_block.x);
		const float padding_border = item.box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
		const float margins_and_frame = item.edges_x;
		const float min_outer = min_width + margins_and_frame;
		item.min_content = Math::Max(item.min_content, min_outer);
		item.max_content = Math::Max(item.max_content, item.min_content);
		if (max_width < FLT_MAX)
		{
			item.max_content = Math::Min(item.max_content, max_width + margins_and_frame);
			item.min_content = Math::Min(item.min_content, item.max_content);
		}
		(void)padding_border;
	};

	auto needs_content = [](const Vector<Track>& tracks, const Placement& placement) {
		for (int i = placement.start; i < placement.start + placement.span && i < (int)tracks.size(); i++)
			if (tracks[i].def.min.IsIntrinsic() || tracks[i].def.max.IsIntrinsic() || tracks[i].def.max.kind == Sizing::Fr)
				return true;
		return false;
	};

	const auto justify_content = computed.justify_content();
	const auto align_content = computed.align_content();
	{
		Vector<AxisItem> axis_items;
		for (GridItem& item : items)
		{
			AxisItem axis_item;
			axis_item.start = item.column.start;
			axis_item.span = item.column.span;
			if (needs_content(columns, item.column))
			{
				content_widths(item);
				axis_item.min_contribution = item.min_content;
				axis_item.max_contribution = item.max_content;
			}
			axis_items.push_back(axis_item);
		}
		SizeTracks(columns, axis_items, available_content_size.x, column_gap, justify_content == Style::JustifyContent::Stretch);
	}
	float used_width = 0.f;
	const Vector<float> column_positions = PositionTracks(columns, column_gap, available_content_size.x, justify_content, used_width);

	auto area_size = [](const Vector<Track>& tracks, const Vector<float>& positions, const Placement& placement) {
		const int last = Math::Min(placement.start + placement.span, (int)tracks.size()) - 1;
		if (last < placement.start)
			return 0.f;
		return positions[last] + tracks[last].size - positions[placement.start];
	};

	const int justify_items = GetKeyword(element_grid, "justify-items", 0);
	const Style::AlignItems align_items = computed.align_items();

	auto justify_of = [&](GridItem& item) {
		int value = GetKeyword(item.element, "justify-self", 0);
		if (value == 0)
			value = justify_items;
		return (value == 0 ? SelfAlign::Normal : SelfAlign(value));
	};
	auto align_of = [&](GridItem& item) {
		SelfAlign align = FromAlignSelf(item.element->GetComputedValues().align_self());
		if (align == SelfAlign::Normal)
			align = FromAlignSelf(static_cast<Style::AlignSelf>(static_cast<int>(align_items) + 1));
		return align;
	};

	// The used content width of each item in its area.
	for (GridItem& item : items)
	{
		const float area_width = area_size(columns, column_positions, item.column);
		const ComputedValues& item_computed = item.element->GetComputedValues();
		const bool auto_margins = (item_computed.margin_left().type == Style::Margin::Auto || item_computed.margin_right().type == Style::Margin::Auto);
		const SelfAlign justify = justify_of(item);
		float width = item.box.GetSize().x;
		if (width < 0.f)
		{
			const bool replaced = item.element->IsReplaced();
			if ((justify == SelfAlign::Normal && !replaced && !auto_margins) || justify == SelfAlign::Stretch)
				width = area_width - item.edges_x;
			else
			{
				content_widths(item);
				width = Math::Min(item.max_content, area_width) - item.edges_x;
			}
			float min_width = 0.f, max_width = FLT_MAX;
			LayoutDetails::GetMinMaxWidth(min_width, max_width, item_computed, item.box, area_width);
			width = Math::Clamp(width, min_width, max_width);
		}
		item.used_width = Math::Max(width, 0.f);
	}

	// -- Row sizes --
	{
		Vector<AxisItem> axis_items;
		for (GridItem& item : items)
		{
			AxisItem axis_item;
			axis_item.start = item.row.start;
			axis_item.span = item.row.span;
			if (needs_content(rows, item.row))
			{
				const float area_width = area_size(columns, column_positions, item.column);
				Box height_box;
				LayoutDetails::BuildBox(height_box, Vector2f(area_width, available_content_size.y), item.element, BuildBoxMode::UnalignedBlock);
				height_box.SetContent(Vector2f(item.used_width, height_box.GetSize().y));
				RootBox root(Vector2f(area_width, available_content_size.y));
				FormattingContext::FormatIndependent(&root, item.element, &height_box, FormattingContextType::Block);
				const Box& formatted = item.element->GetBox();
				item.height = formatted.GetSize(BoxArea::Border).y + formatted.GetEdge(BoxArea::Margin, BoxEdge::Top) +
					formatted.GetEdge(BoxArea::Margin, BoxEdge::Bottom);
				axis_item.min_contribution = axis_item.max_contribution = item.height;
			}
			axis_items.push_back(axis_item);
		}
		SizeTracks(rows, axis_items, available_content_size.y, row_gap, align_content == Style::AlignContent::Stretch);
	}

	static_assert(int(Style::AlignContent::FlexStart) == int(Style::JustifyContent::FlexStart) &&
			int(Style::AlignContent::SpaceEvenly) == int(Style::JustifyContent::SpaceEvenly),
		"align-content and justify-content share their distribution values.");
	const Style::JustifyContent row_distribution =
		(align_content == Style::AlignContent::Stretch ? Style::JustifyContent::Stretch : static_cast<Style::JustifyContent>(align_content));
	float used_height = 0.f;
	const Vector<float> row_positions = PositionTracks(rows, row_gap, available_content_size.y, row_distribution, used_height);

	// -- Format and position the items --
	bool baseline_set = false;
	for (GridItem& item : items)
	{
		const Vector2f area_position = {column_positions[Math::Min(item.column.start, (int)columns.size())],
			row_positions[Math::Min(item.row.start, (int)rows.size())]};
		const Vector2f area = {area_size(columns, column_positions, item.column), area_size(rows, row_positions, item.row)};
		const ComputedValues& item_computed = item.element->GetComputedValues();

		Box box;
		LayoutDetails::BuildBox(box, area, item.element, BuildBoxMode::UnalignedBlock);
		float height = box.GetSize().y;
		const SelfAlign align = align_of(item);
		const bool auto_margin_top = (item_computed.margin_top().type == Style::Margin::Auto);
		const bool auto_margin_bottom = (item_computed.margin_bottom().type == Style::Margin::Auto);
		if (height < 0.f && (align == SelfAlign::Stretch || align == SelfAlign::Normal) && !item.element->IsReplaced() && !auto_margin_top &&
			!auto_margin_bottom)
		{
			float min_height = 0.f, max_height = FLT_MAX;
			LayoutDetails::GetMinMaxHeight(min_height, max_height, item_computed, box, area.y);
			height = Math::Clamp(area.y - item.edges_y, min_height, max_height);
		}
		box.SetContent(Vector2f(item.used_width, height));

		UniquePtr<LayoutBox> layout_box = FormattingContext::FormatIndependent(container_box, item.element, &box, FormattingContextType::Block);
		const Box& formatted = item.element->GetBox();
		const Vector2f border_size = formatted.GetSize(BoxArea::Border);
		const float margin_left = formatted.GetEdge(BoxArea::Margin, BoxEdge::Left);
		const float margin_right = formatted.GetEdge(BoxArea::Margin, BoxEdge::Right);
		const float margin_top = formatted.GetEdge(BoxArea::Margin, BoxEdge::Top);
		const float margin_bottom = formatted.GetEdge(BoxArea::Margin, BoxEdge::Bottom);

		Vector2f offset = area_position + Vector2f(margin_left, margin_top);
		const float free_x = area.x - (border_size.x + margin_left + margin_right);
		const float free_y = area.y - (border_size.y + margin_top + margin_bottom);
		const bool auto_margin_left = (item_computed.margin_left().type == Style::Margin::Auto);
		const bool auto_margin_right = (item_computed.margin_right().type == Style::Margin::Auto);
		if (free_x > 0.f)
		{
			if (auto_margin_left && auto_margin_right)
				offset.x += 0.5f * free_x;
			else if (auto_margin_left)
				offset.x += free_x;
			else if (!auto_margin_right)
			{
				const SelfAlign justify = justify_of(item);
				if (justify == SelfAlign::End)
					offset.x += free_x;
				else if (justify == SelfAlign::Center)
					offset.x += 0.5f * free_x;
			}
		}
		if (free_y > 0.f)
		{
			if (auto_margin_top && auto_margin_bottom)
				offset.y += 0.5f * free_y;
			else if (auto_margin_top)
				offset.y += free_y;
			else if (!auto_margin_bottom)
			{
				if (align == SelfAlign::End)
					offset.y += free_y;
				else if (align == SelfAlign::Center)
					offset.y += 0.5f * free_y;
			}
		}

		item.element->SetOffset(content_offset + offset, element_grid);

		if (layout_box && !baseline_set && layout_box->GetBaselineOfLastLine(baseline))
		{
			baseline += content_offset.y + offset.y;
			baseline_set = true;
		}

		if (layout_box)
			visible_overflow_size = Math::Max(visible_overflow_size, offset + layout_box->GetVisibleOverflowSize());
		scrollable_content_size = Math::Max(scrollable_content_size, offset + border_size + Vector2f(margin_right, margin_bottom));
	}

	resulting_content_size = Vector2f(available_content_size.x >= 0.f ? available_content_size.x : used_width,
		available_content_size.y >= 0.f ? available_content_size.y : used_height);
	if (available_content_size.x >= 0.f)
		resulting_content_size.x = available_content_size.x;
}

} // namespace Rml
