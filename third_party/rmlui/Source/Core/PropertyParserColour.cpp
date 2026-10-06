#include "PropertyParserColour.h"
#include "ControlledLifetimeResource.h"
#include <algorithm>
#include <cmath>
#include <string.h>
#include <utility>

namespace Rml {

// Helper function for hsl->rgb conversion.
static float HSL_f(float h, float s, float l, float n)
{
	float k = std::fmod((n + h * (1.0f / 30.0f)), 12.0f);
	float a = s * std::min(l, 1.0f - l);
	return l - a * std::max(-1.0f, std::min({k - 3.0f, 9.0f - k, 1.0f}));
}

// Reference: https://en.wikipedia.org/wiki/HSL_and_HSV#HSL_to_RGB_alternative
static void HSLAToRGBA(Array<float, 4>& vals)
{
	if (vals[1] == 0.0f)
	{
		vals[0] = vals[1] = vals[2];
	}
	else
	{
		float h = std::fmod(vals[0], 360.0f);
		if (h < 0)
			h += 360.0f;
		float s = vals[1];
		float l = vals[2];
		vals[0] = HSL_f(h, s, l, 0.0f);
		vals[1] = HSL_f(h, s, l, 8.0f);
		vals[2] = HSL_f(h, s, l, 4.0f);
	}
}

// Reference: https://en.wikipedia.org/wiki/SRGB#Definition
static float InverseSRGBNonlinearTransfer(float channel)
{
	return channel > 0.0031308f ? 1.055f * std::pow(channel, 1.0f / 2.4f) - 0.055f : 12.92f * channel;
}

// Reference: https://en.wikipedia.org/wiki/CIELAB_color_space#Converting_between_CIELAB_and_CIE_XYZ_coordinates
static void CIELABToRGBA(Array<float, 4>& values)
{
	float y_double_prime = (values[0] + 16.0f) / 116.0f;
	float x_double_prime = (values[1] / 500.0f) + y_double_prime;
	float z_double_prime = y_double_prime - (values[2] / 200.0f);

	float x_prime = (x_double_prime * x_double_prime * x_double_prime) > 0.008856f ? (x_double_prime * x_double_prime * x_double_prime)
																				   : (x_double_prime - (16.0f / 116.0f)) / 7.787f;
	float y_prime = (y_double_prime * y_double_prime * y_double_prime) > 0.008856f ? (y_double_prime * y_double_prime * y_double_prime)
																				   : (y_double_prime - (16.0f / 116.0f)) / 7.787f;
	float z_prime = (z_double_prime * z_double_prime * z_double_prime) > 0.008856f ? (z_double_prime * z_double_prime * z_double_prime)
																				   : (z_double_prime - (16.0f / 116.0f)) / 7.787f;

	static const Vector3f illuminant_d65_multiplicands(0.95047f, 1.0f, 1.08883f);

	float x = x_prime * illuminant_d65_multiplicands.x;
	float y = y_prime * illuminant_d65_multiplicands.y;
	float z = z_prime * illuminant_d65_multiplicands.z;

	static constexpr Array<Array<float, 3>, 3> xyz_to_srgb_matrix{
		Array<float, 3>{+3.2404548f, -1.5371389f, -0.4985315f},
		Array<float, 3>{-0.9692664f, +1.8760109f, +0.0415561f},
		Array<float, 3>{+0.0556434f, -0.2040259f, +1.0572252f},
	};

	float r = xyz_to_srgb_matrix[0][0] * x + xyz_to_srgb_matrix[0][1] * y + xyz_to_srgb_matrix[0][2] * z;
	float g = xyz_to_srgb_matrix[1][0] * x + xyz_to_srgb_matrix[1][1] * y + xyz_to_srgb_matrix[1][2] * z;
	float b = xyz_to_srgb_matrix[2][0] * x + xyz_to_srgb_matrix[2][1] * y + xyz_to_srgb_matrix[2][2] * z;

	values[0] = Math::Clamp(InverseSRGBNonlinearTransfer(r), 0.0f, 1.0f);
	values[1] = Math::Clamp(InverseSRGBNonlinearTransfer(g), 0.0f, 1.0f);
	values[2] = Math::Clamp(InverseSRGBNonlinearTransfer(b), 0.0f, 1.0f);
}

// References: https://en.wikipedia.org/wiki/Oklab_color_space#Conversions_between_color_spaces and https://bottosson.github.io/posts/oklab/
static void OklabToRGBA(Array<float, 4>& values)
{
	static constexpr Array<Array<float, 3>, 3> oklab_to_lms_prime_matrix{
		Array<float, 3>{+1.0f, +0.3963377774f, +0.2158037573f},
		Array<float, 3>{+1.0f, -0.1055613458f, -0.0638541728f},
		Array<float, 3>{+1.0f, -0.0894841775f, -1.2914855480f},
	};

	float lightness = values[0];
	float a_axis = values[1];
	float b_axis = values[2];

	float l_prime = oklab_to_lms_prime_matrix[0][0] * lightness + oklab_to_lms_prime_matrix[0][1] * a_axis + oklab_to_lms_prime_matrix[0][2] * b_axis;
	float m_prime = oklab_to_lms_prime_matrix[1][0] * lightness + oklab_to_lms_prime_matrix[1][1] * a_axis + oklab_to_lms_prime_matrix[1][2] * b_axis;
	float s_prime = oklab_to_lms_prime_matrix[2][0] * lightness + oklab_to_lms_prime_matrix[2][1] * a_axis + oklab_to_lms_prime_matrix[2][2] * b_axis;

	float l = l_prime * l_prime * l_prime;
	float m = m_prime * m_prime * m_prime;
	float s = s_prime * s_prime * s_prime;

	static constexpr Array<Array<float, 3>, 3> lms_to_srgb_matrix{
		Array<float, 3>{+4.0767416621f, -3.3077115913f, +0.2309699292f},
		Array<float, 3>{-1.2684380046f, +2.6097574011f, -0.3413193965f},
		Array<float, 3>{-0.0041960863f, -0.7034186147f, +1.7076147010f},
	};

	float r = lms_to_srgb_matrix[0][0] * l + lms_to_srgb_matrix[0][1] * m + lms_to_srgb_matrix[0][2] * s;
	float g = lms_to_srgb_matrix[1][0] * l + lms_to_srgb_matrix[1][1] * m + lms_to_srgb_matrix[1][2] * s;
	float b = lms_to_srgb_matrix[2][0] * l + lms_to_srgb_matrix[2][1] * m + lms_to_srgb_matrix[2][2] * s;

	values[0] = Math::Clamp(InverseSRGBNonlinearTransfer(r), 0.0f, 1.0f);
	values[1] = Math::Clamp(InverseSRGBNonlinearTransfer(g), 0.0f, 1.0f);
	values[2] = Math::Clamp(InverseSRGBNonlinearTransfer(b), 0.0f, 1.0f);
}

struct PropertyParserColourData {
	const UnorderedMap<String, Colourb> html_colours = {
		{"black", Colourb(0, 0, 0)},
		{"silver", Colourb(192, 192, 192)},
		{"gray", Colourb(128, 128, 128)},
		{"grey", Colourb(128, 128, 128)},
		{"white", Colourb(255, 255, 255)},
		{"maroon", Colourb(128, 0, 0)},
		{"red", Colourb(255, 0, 0)},
		{"orange", Colourb(255, 165, 0)},
		{"purple", Colourb(128, 0, 128)},
		{"fuchsia", Colourb(255, 0, 255)},
		{"green", Colourb(0, 128, 0)},
		{"lime", Colourb(0, 255, 0)},
		{"olive", Colourb(128, 128, 0)},
		{"yellow", Colourb(255, 255, 0)},
		{"navy", Colourb(0, 0, 128)},
		{"blue", Colourb(0, 0, 255)},
		{"teal", Colourb(0, 128, 128)},
		{"aqua", Colourb(0, 255, 255)},
		{"transparent", Colourb(0, 0, 0, 0)},
	};
};

ControlledLifetimeResource<PropertyParserColourData> PropertyParserColour::parser_data;

void PropertyParserColour::Initialize()
{
	parser_data.Initialize();
}
void PropertyParserColour::Shutdown()
{
	parser_data.Shutdown();
}

PropertyParserColour::PropertyParserColour() {}

PropertyParserColour::~PropertyParserColour() {}

bool PropertyParserColour::ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const
{
	Colourb colour;
	if (!ParseColour(colour, value))
		return false;

	property.value = Variant(colour);
	property.unit = Unit::COLOUR;

	return true;
}

bool PropertyParserColour::ParseColour(Colourb& colour, const String& in_value)
{
	if (in_value.empty())
		return false;

	colour = {};

	// Forge: function names and keywords are case-insensitive, e.g. Bootstrap writes RGBA(...).
	const String value = (in_value[0] == '#' ? in_value : StringUtilities::ToLower(in_value));

	if (value[0] == '#')
	{
		if (!ParseHexColour(colour, value))
			return false;
	}
	else if (value.substr(0, 3) == "rgb")
	{
		if (!ParseRGBColour(colour, value))
			return false;
	}
	else if (value.substr(0, 3) == "hsl" || value.substr(0, 3) == "hwb")
	{
		if (!ParseHSLColour(colour, value))
			return false;
	}
	else if (value.substr(0, 3) == "lab" || value.substr(0, 3) == "lch")
	{
		if (!ParseCIELABColour(colour, value))
			return false;
	}
	else if (value.substr(0, 5) == "oklab" || value.substr(0, 5) == "oklch")
	{
		if (!ParseOklabColour(colour, value))
			return false;
	}
	else if (value.substr(0, 10) == "color-mix(")
	{
		if (!ParseColourMix(colour, value))
			return false;
	}
	else
	{
		// Check for the specification of an HTML colour.
		auto it = parser_data->html_colours.find(value);
		if (it == parser_data->html_colours.end())
			return false;
		else
			colour = it->second;
	}

	return true;
}

// Forge: reads the arguments of rgb()/hsl()/hwb() in both the legacy comma syntax and the modern space syntax with '/ alpha'.
// Each argument is returned as a number; 'percent' tells which ones had a '%' sign. Hue angle units are converted to degrees.
static bool GetModernColourArguments(const String& value, Array<float, 4>& numbers, Array<bool, 4>& percent, int& count)
{
	const size_t open = value.find('(');
	const size_t close = value.rfind(')');
	if (open == String::npos || close == String::npos || close < open)
		return false;

	String args = value.substr(open + 1, close - open - 1);
	for (char& c : args)
		if (c == ',' || c == '/')
			c = ' ';

	StringList list;
	StringUtilities::ExpandString(list, args, ' ', true);
	count = 0;
	for (const String& item : list)
	{
		if (item.empty())
			continue;
		if (count >= 4)
			return false;
		if (item == "none")
		{
			numbers[count] = 0.f;
			percent[count] = false;
			count++;
			continue;
		}
		char* end = nullptr;
		const float number = strtof(item.c_str(), &end);
		if (end == item.c_str())
			return false;
		const String unit = end;
		float scale = 1.f;
		percent[count] = (unit == "%");
		if (unit == "turn")
			scale = 360.f;
		else if (unit == "rad")
			scale = 57.2957795f;
		else if (unit == "grad")
			scale = 0.9f;
		else if (!unit.empty() && unit != "%" && unit != "deg")
			return false;
		numbers[count] = number * scale;
		count++;
	}
	return count == 3 || count == 4;
}

static float AlphaFromArgument(const Array<float, 4>& numbers, const Array<bool, 4>& percent, int count)
{
	if (count < 4)
		return 1.f;
	return Math::Clamp(percent[3] ? numbers[3] * 0.01f : numbers[3], 0.f, 1.f);
}

bool PropertyParserColour::ParseColourMix(Colourb& colour, const String& value)
{
	// color-mix(in <space>, <colour> [<percentage>], <colour> [<percentage>]). Mixed in sRGB with premultiplied alpha.
	const size_t open = value.find('(');
	const size_t close = value.rfind(')');
	if (close == String::npos || close < open)
		return false;
	StringList parts;
	StringUtilities::ExpandString(parts, value.substr(open + 1, close - open - 1), ',', '(', ')');
	if (parts.size() != 3 || parts[0].substr(0, 2) != "in")
		return false;

	Colourb colours[2];
	float weights[2] = {-1.f, -1.f};
	for (int i = 0; i < 2; i++)
	{
		String part = StringUtilities::StripWhitespace(parts[i + 1]);
		// A trailing percentage outside any parentheses is the weight.
		const size_t space = part.rfind(' ');
		if (space != String::npos && !part.empty() && part.back() == '%' && part.find(')', space) == String::npos)
		{
			weights[i] = (float)atof(part.substr(space + 1).c_str()) * 0.01f;
			part = StringUtilities::StripWhitespace(part.substr(0, space));
		}
		if (!ParseColour(colours[i], part))
			return false;
	}
	if (weights[0] < 0.f && weights[1] < 0.f)
		weights[0] = weights[1] = 0.5f;
	else if (weights[0] < 0.f)
		weights[0] = 1.f - weights[1];
	else if (weights[1] < 0.f)
		weights[1] = 1.f - weights[0];

	const float sum = weights[0] + weights[1];
	if (sum <= 0.f)
		return false;
	const float alpha_scale = std::min(sum, 1.f);
	const float w0 = weights[0] / sum, w1 = weights[1] / sum;

	const float a0 = colours[0].alpha / 255.f, a1 = colours[1].alpha / 255.f;
	const float alpha = a0 * w0 + a1 * w1;
	for (int c = 0; c < 3; c++)
	{
		const float premultiplied = colours[0][c] * a0 * w0 + colours[1][c] * a1 * w1;
		colour[c] = (byte)Math::Clamp((int)std::lround(alpha > 0.f ? premultiplied / alpha : 0.f), 0, 255);
	}
	colour.alpha = (byte)Math::Clamp((int)std::lround(alpha * alpha_scale * 255.f), 0, 255);
	return true;
}

bool PropertyParserColour::ParseHexColour(Colourb& colour, const String& value)
{
	char hex_values[4][2] = {{'f', 'f'}, {'f', 'f'}, {'f', 'f'}, {'f', 'f'}};

	switch (value.size())
	{
	// Single hex digit per channel, RGB and alpha.
	case 5:
		hex_values[3][0] = hex_values[3][1] = value[4];
		//-fallthrough
	// Single hex digit per channel, RGB only.
	case 4:
		hex_values[0][0] = hex_values[0][1] = value[1];
		hex_values[1][0] = hex_values[1][1] = value[2];
		hex_values[2][0] = hex_values[2][1] = value[3];
		break;

	// Two hex digits per channel, RGB and alpha.
	case 9:
		hex_values[3][0] = value[7];
		hex_values[3][1] = value[8];
		//-fallthrough
	// Two hex digits per channel, RGB only.
	case 7: memcpy(hex_values, &value.c_str()[1], sizeof(char) * 6); break;

	default: return false;
	}

	// Parse each of the colour elements.
	for (int i = 0; i < 4; i++)
	{
		int tens = Math::HexToDecimal(hex_values[i][0]);
		int ones = Math::HexToDecimal(hex_values[i][1]);
		if (tens == -1 || ones == -1)
			return false;

		colour[i] = (byte)(tens * 16 + ones);
	}

	return true;
}

bool PropertyParserColour::ParseRGBColour(Colourb& colour, const String& value)
{
	// Forge: rgb() and rgba() are the same function, in comma or space syntax.
	Array<float, 4> numbers;
	Array<bool, 4> percent;
	int count = 0;
	if (!GetModernColourArguments(value, numbers, percent, count))
		return false;

	for (int i = 0; i < 3; ++i)
	{
		const float component = (percent[i] ? numbers[i] * (255.0f / 100.0f) : numbers[i]);
		colour[i] = (byte)Math::Clamp((int)std::lround(component), 0, 255);
	}
	colour[3] = (byte)std::lround(AlphaFromArgument(numbers, percent, count) * 255.f);

	return true;
}

bool PropertyParserColour::ParseHSLColour(Colourb& colour, const String& value)
{
	// Forge: hsl(), hsla() and hwb(), in comma or space syntax.
	Array<float, 4> numbers;
	Array<bool, 4> percent;
	int count = 0;
	if (!GetModernColourArguments(value, numbers, percent, count))
		return false;

	Array<float, 4> vals;
	vals[0] = numbers[0];
	vals[1] = Math::Clamp(numbers[1] * 0.01f, 0.f, 1.f);
	vals[2] = Math::Clamp(numbers[2] * 0.01f, 0.f, 1.f);
	vals[3] = AlphaFromArgument(numbers, percent, count);

	if (value.substr(0, 3) == "hwb")
	{
		float white = vals[1], black = vals[2];
		if (white + black >= 1.f)
		{
			const float grey = white / (white + black);
			vals[0] = vals[1] = vals[2] = grey;
		}
		else
		{
			Array<float, 4> rgb = {vals[0], 1.f, 0.5f, vals[3]};
			HSLAToRGBA(rgb);
			for (int i = 0; i < 3; i++)
				vals[i] = rgb[i] * (1.f - white - black) + white;
		}
	}
	else
		HSLAToRGBA(vals);

	for (int i = 0; i < 4; ++i)
		colour[i] = (byte)(Math::Clamp((int)std::lround(vals[i] * 255.0f), 0, 255));

	return true;
}

bool PropertyParserColour::ParseCIELABColour(Colourb& colour, const String& value)
{
	StringList values;
	values.reserve(5);
	if (!GetColourFunctionValues(values, value, false))
		return false;

	// Check if we have an alpha component.
	if (values.size() == 5)
	{
		if (values[3] != "/")
			return false;

		values[3] = std::move(values[4]);
		values.pop_back();
	}
	else
	{
		if (values.size() != 3)
			return false;

		values.push_back("1.0");
	}

	Array<float, 4> lab_values;

	// Parse lightness and alpha (same for both lab and lch).
	for (int i : {0, 3})
	{
		// Value can either be 'none' (representing 0.0), a percentage between 0% and 100%, or a number (between 0.0 and 100.0 for lightness and between 0.0 and 1.0 for alpha).
		if (values[i] == "none")
			lab_values[i] = 0.0f;
		else if (values[i][values[i].size() - 1] == '%')
		{
			lab_values[i] = (float)atof(values[i].substr(0, values[i].size() - 1).c_str());
			if (i == 3)
				lab_values[i] /= 100.0f;
		}
		else
			lab_values[i] = (float)atof(values[i].c_str());

		lab_values[i] = Math::Clamp(lab_values[i], 0.0f, i == 0 ? 100.0f : 1.0f);
	}

	// Determine if colour is in CIELAB or CIELCh space.
	if (value.substr(0, 3) == "lab")
	{
		// Parse A-axis (green-to-red) and B-axis (blue-to-yellow).
		for (int i : {1, 2})
		{
			// Value can either be 'none' (representing 0.0), a percentage between -100% and +100% (representing -125.0 to +125.0), or a number.
			if (values[i] == "none")
				lab_values[i] = 0.0f;
			else if (values[i][values[i].size() - 1] == '%')
			{
				static constexpr float cielab_axis_percentage_bound = 125.0f;
				lab_values[i] = (float)atof(values[i].substr(0, values[i].size() - 1).c_str()) / 100.0f * cielab_axis_percentage_bound;
			}
			else
				lab_values[i] = (float)atof(values[i].c_str());

			// Whilst the axis values are theoretically unbounded, in practice, they only exist between -160.0 and +160.0.
			static constexpr float cielab_axis_bound_limit = 160.0f;
			lab_values[i] = Math::Clamp(lab_values[i], -cielab_axis_bound_limit, +cielab_axis_bound_limit);
		}
	}
	else
	{
		// Parse chroma; value can either be 'none' (representing 0.0), a percentage between 0% and 100% (representing 0.0 to 150.0), or a number.
		float chroma = 0.0f;
		if (values[1] == "none")
			chroma = 0.0f;
		else if (values[1][values[1].size() - 1] == '%')
		{
			static constexpr float cielch_maximum_percentage_chroma = 150.0f;
			chroma = (float)atof(values[1].substr(0, values[1].size() - 1).c_str()) / 100.0f * cielch_maximum_percentage_chroma;
		}
		else
			chroma = (float)atof(values[1].c_str());

		// Whilst the chroma is theoretically unbounded, in practice, it does not exceed 230.0.
		static constexpr float cielch_maximum_chroma = 230.0f;
		chroma = Math::Clamp(chroma, 0.0f, cielch_maximum_chroma);

		// Parse hue; value can either be 'none' (representing 0.0), or an angle.
		float hue = 0.0f;
		if (values[2] == "none")
			hue = 0.0f;
		else
			hue = (float)atof(values[2].c_str());

		// Convert LCh polar coordinates to LAB Cartesian coordinates.
		lab_values[1] = chroma * Math::Cos(Math::DegreesToRadians(hue));
		lab_values[2] = chroma * Math::Sin(Math::DegreesToRadians(hue));
	}

	CIELABToRGBA(lab_values);
	for (int i = 0; i < 4; ++i)
		colour[i] = (byte)(Math::Clamp((int)(lab_values[i] * 255.0f), 0, 255));

	return true;
}

bool PropertyParserColour::ParseOklabColour(Colourb& colour, const String& value)
{
	StringList values;
	values.reserve(5);
	if (!GetColourFunctionValues(values, value, false))
		return false;

	// Check if we have an alpha component.
	if (values.size() == 5)
	{
		if (values[3] != "/")
			return false;

		values[3] = std::move(values[4]);
		values.pop_back();
	}
	else
	{
		if (values.size() != 3)
			return false;

		values.push_back("1.0");
	}

	Array<float, 4> oklab_values;

	// Parse lightness and alpha (same for both Oklab and Oklch).
	for (int i : {0, 3})
	{
		// Value can either be 'none' (representing 0.0), a percentage between 0% and 100%, or a number between 0.0 and 1.0.
		if (values[i] == "none")
			oklab_values[i] = 0.0f;
		else if (values[i][values[i].size() - 1] == '%')
			oklab_values[i] = (float)atof(values[i].substr(0, values[i].size() - 1).c_str()) / 100.0f;
		else
			oklab_values[i] = (float)atof(values[i].c_str());

		oklab_values[i] = Math::Clamp(oklab_values[i], 0.0f, 1.0f);
	}

	// Determine if colour is in Oklab or Oklch space.
	if (value.substr(0, 5) == "oklab")
	{
		// Parse A-axis (green-to-red) and B-axis (blue-to-yellow).
		for (int i : {1, 2})
		{
			// Value can either be 'none' (representing 0.0), a percentage between -100% and +100% (representing -0.4 to +0.4), or a number.
			if (values[i] == "none")
				oklab_values[i] = 0.0f;
			else if (values[i][values[i].size() - 1] == '%')
			{
				static constexpr float oklab_axis_percentage_bound = 0.4f;
				oklab_values[i] = (float)atof(values[i].substr(0, values[i].size() - 1).c_str()) / 100.0f * oklab_axis_percentage_bound;
			}
			else
				oklab_values[i] = (float)atof(values[i].c_str());

			// Whilst the axis values are theoretically unbounded, in practice, they only exist between -0.5 and +0.5.
			static constexpr float oklab_axis_bound_limit = 0.5f;
			oklab_values[i] = Math::Clamp(oklab_values[i], -oklab_axis_bound_limit, +oklab_axis_bound_limit);
		}
	}
	else
	{
		// Parse chroma; value can either be 'none' (representing 0.0), a percentage between 0% and 100% (representing 0.0 to 0.4), or a number.
		float chroma = 0.0f;
		if (values[1] == "none")
			chroma = 0.0f;
		else if (values[1][values[1].size() - 1] == '%')
		{
			static constexpr float oklch_maximum_percentage_chroma = 0.4f;
			chroma = (float)atof(values[1].substr(0, values[1].size() - 1).c_str()) / 100.0f * oklch_maximum_percentage_chroma;
		}
		else
			chroma = (float)atof(values[1].c_str());

		// Whilst the chroma is theoretically unbounded, in practice, it does not exceed 0.5.
		static constexpr float oklch_maximum_chroma = 0.5f;
		chroma = Math::Clamp(chroma, 0.0f, oklch_maximum_chroma);

		// Parse hue; value can either be 'none' (representing 0.0), or an angle.
		float hue = 0.0f;
		if (values[2] == "none")
			hue = 0.0f;
		else
			hue = (float)atof(values[2].c_str());

		// Convert Oklch polar coordinates to Oklab Cartesian coordinates.
		oklab_values[1] = chroma * Math::Cos(Math::DegreesToRadians(hue));
		oklab_values[2] = chroma * Math::Sin(Math::DegreesToRadians(hue));
	}

	OklabToRGBA(oklab_values);
	for (int i = 0; i < 4; ++i)
		colour[i] = (byte)(Math::Clamp((int)(oklab_values[i] * 255.0f), 0, 255));

	return true;
}

bool PropertyParserColour::GetColourFunctionValues(StringList& values, const String& value, bool is_comma_separated)
{
	size_t find = value.find('(');
	if (find == String::npos)
		return false;

	size_t begin_values = find + 1;

	StringUtilities::ExpandString(values, value.substr(begin_values, value.rfind(')') - begin_values), is_comma_separated ? ',' : ' ',
		!is_comma_separated);

	return true;
}

} // namespace Rml
