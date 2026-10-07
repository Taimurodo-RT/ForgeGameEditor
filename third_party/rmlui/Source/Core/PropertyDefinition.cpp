#include <algorithm>
#include <cctype>
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/Log.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "CalcExpression.h"

namespace Rml {

PropertyDefinition::PropertyDefinition(PropertyId id, const String& _default_value, bool _inherited, bool _forces_layout) :
	id(id), default_value(_default_value, Unit::UNKNOWN), relative_target(RelativeTarget::None)
{
	inherited = _inherited;
	forces_layout = _forces_layout;
	default_value.definition = this;
}

PropertyDefinition::~PropertyDefinition() {}

PropertyDefinition& PropertyDefinition::AddParser(const String& parser_name, const String& parser_parameters)
{
	ParserState new_parser;

	// Fetch the parser.
	new_parser.parser = StyleSheetSpecification::GetParser(parser_name);
	if (new_parser.parser == nullptr)
	{
		Log::Message(Log::LT_ERROR, "Property was registered with invalid parser '%s'.", parser_name.c_str());
		return *this;
	}

	// Split the parameter list, and set up the map.
	if (!parser_parameters.empty())
	{
		StringList parameter_list;
		StringUtilities::ExpandString(parameter_list, parser_parameters);

		int parameter_value = 0;
		for (const String& parameter : parameter_list)
		{
			// Look for an optional parameter value such as in "normal=400".
			const size_t i_equal = parameter.find('=');
			if (i_equal != String::npos)
			{
				if (!TypeConverter<String, int>::Convert(parameter.substr(i_equal + 1), parameter_value))
				{
					Log::Message(Log::LT_ERROR, "Parser was added with invalid parameter '%s'.", parameter.c_str());
					return *this;
				}
			}

			new_parser.parameters[parameter.substr(0, i_equal)] = parameter_value;
			parameter_value += 1;
		}
	}

	const int parser_index = (int)parsers.size();
	parsers.push_back(new_parser);

	// If the default value has not been parsed successfully yet, run it through the new parser.
	if (default_value.unit == Unit::UNKNOWN)
	{
		String unparsed_value = default_value.value.Get<String>();
		if (new_parser.parser->ParseValue(default_value, unparsed_value, new_parser.parameters))
		{
			default_value.parser_index = parser_index;
		}
		else
		{
			default_value.value = unparsed_value;
			default_value.unit = Unit::UNKNOWN;
		}
	}

	return *this;
}

bool PropertyDefinition::ParseValue(Property& property, const String& value) const
{
	// Forge: CSS-wide keywords and currentColor are kept as text and resolved when the element's values are computed.
	{
		const String lower = StringUtilities::ToLower(StringUtilities::StripWhitespace(value));
		bool deferred = Calc::IsCssWideKeyword(lower);
		if (!deferred && lower.find("currentcolor") != String::npos)
		{
			Property probe;
			if (!ParseValue(probe, StringUtilities::Replace(lower, "currentcolor", "black")))
			{
				property.unit = Unit::UNKNOWN;
				return false;
			}
			deferred = true;
		}
		if (deferred)
		{
			property.value = lower;
			property.unit = Unit::CALC;
			property.definition = this;
			property.parser_index = -1;
			return true;
		}
	}

	// Forge: units without a native representation (vmin, vmax, ch, ex) are evaluated like calc().
	if (!Calc::ContainsMath(value))
	{
		static const char* const wrapped_units[] = {"vmin", "vmax", "ch", "ex"};
		String rewritten;
		bool changed = false;
		for (size_t i = 0; i < value.size();)
		{
			const bool starts_number = (value[i] >= '0' && value[i] <= '9') || (value[i] == '.' && i + 1 < value.size() && value[i + 1] >= '0' && value[i + 1] <= '9');
			const bool after_ident = (i > 0 && (std::isalnum((unsigned char)value[i - 1]) || value[i - 1] == '-' || value[i - 1] == '#' || value[i - 1] == '_'));
			if (!starts_number || (after_ident && value[i - 1] != '-'))
			{
				rewritten += value[i++];
				continue;
			}
			size_t end = i;
			while (end < value.size() && ((value[end] >= '0' && value[end] <= '9') || value[end] == '.'))
				end++;
			size_t unit_end = end;
			while (unit_end < value.size() && std::isalpha((unsigned char)value[unit_end]))
				unit_end++;
			const String unit = StringUtilities::ToLower(value.substr(end, unit_end - end));
			const bool wrap = std::any_of(std::begin(wrapped_units), std::end(wrapped_units), [&](const char* u) { return unit == u; });
			const bool negative = (!rewritten.empty() && rewritten.back() == '-' && (rewritten.size() == 1 || !std::isalnum((unsigned char)rewritten[rewritten.size() - 2])));
			if (wrap)
			{
				if (negative)
					rewritten.pop_back();
				rewritten += "calc(" + String(negative ? "-" : "") + value.substr(i, unit_end - i) + ")";
				changed = true;
			}
			else
				rewritten += value.substr(i, unit_end - i);
			i = unit_end;
		}
		if (changed)
			return ParseValue(property, rewritten);
	}

	// Forge: fold math functions into plain values; keep the ones mixing per-element units for compute time.
	if (Calc::ContainsMath(value))
	{
		String folded;
		bool unresolved = false;
		if (!Calc::Fold(value, folded, unresolved))
		{
			property.unit = Unit::UNKNOWN;
			return false;
		}
		if (!unresolved)
			return ParseValue(property, folded);

		const String stripped = StringUtilities::StripWhitespace(folded);
		Property probe;
		bool single_call = (!stripped.empty() && stripped.back() == ')');
		for (size_t i = 0, depth = 0; single_call && i < stripped.size(); i++)
		{
			if (stripped[i] == '(')
				depth++;
			else if (stripped[i] == ')' && --depth == 0 && i + 1 != stripped.size())
				single_call = false;
		}
		if (single_call && ParseValue(probe, "1px"))
		{
			property.value = stripped;
			property.unit = Unit::CALC;
			property.definition = this;
			property.parser_index = -1;
			return true;
		}
		// Properties kept as text (such as background-position) take the math functions as they are.
		for (size_t i = 0; i < parsers.size(); i++)
		{
			if (parsers[i].parser->ParseValue(property, folded, parsers[i].parameters) && property.unit == Unit::STRING)
			{
				property.definition = this;
				property.parser_index = (int)i;
				return true;
			}
		}
		property.unit = Unit::UNKNOWN;
		return false;
	}

	for (size_t i = 0; i < parsers.size(); i++)
	{
		if (parsers[i].parser->ParseValue(property, value, parsers[i].parameters))
		{
			property.definition = this;
			property.parser_index = (int)i;
			return true;
		}
	}

	property.unit = Unit::UNKNOWN;
	return false;
}

bool PropertyDefinition::GetValue(String& value, const Property& property) const
{
	value = property.value.Get<String>();

	switch (property.unit)
	{
	case Unit::KEYWORD:
	{
		int parser_index = property.parser_index;
		if (parser_index < 0 || parser_index >= (int)parsers.size())
		{
			// Look for the keyword parser in the property's list of parsers
			const auto* keyword_parser = StyleSheetSpecification::GetParser("keyword");
			for (int i = 0; i < (int)parsers.size(); i++)
			{
				if (parsers[i].parser == keyword_parser)
				{
					parser_index = i;
					break;
				}
			}
			// If we couldn't find it, exit now
			if (parser_index < 0 || parser_index >= (int)parsers.size())
				return false;
		}

		int keyword = property.value.Get<int>();
		for (const auto& name_keyword : parsers[parser_index].parameters)
		{
			if (name_keyword.second == keyword)
			{
				value = name_keyword.first;
				break;
			}
		}

		return false;
	}
	break;

	default: value += ToString(property.unit); break;
	}

	return true;
}

bool PropertyDefinition::IsInherited() const
{
	return inherited;
}

bool PropertyDefinition::IsLayoutForced() const
{
	return forces_layout;
}

const Property* PropertyDefinition::GetDefaultValue() const
{
	return &default_value;
}

RelativeTarget PropertyDefinition::GetRelativeTarget() const
{
	return relative_target;
}

PropertyId PropertyDefinition::GetId() const
{
	return id;
}

PropertyDefinition& PropertyDefinition::SetRelativeTarget(RelativeTarget relative_target)
{
	this->relative_target = relative_target;
	return *this;
}

} // namespace Rml
