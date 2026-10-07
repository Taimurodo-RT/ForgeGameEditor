#include "CalcExpression.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Rml {
namespace Calc {

namespace {

	// The kinds of units an expression can combine. Pixels-per-inch units are stored as density-independent pixels, like RmlUi does.
	enum Channel { NUMBER, PX, PERCENT, EM, REM, VW, VH, VMIN, VMAX, DP, DEG, SEC, CHANNEL_COUNT };

	struct Value {
		float c[CHANNEL_COUNT] = {};

		int CountNonZero(int* last = nullptr) const
		{
			int n = 0;
			for (int i = 0; i < CHANNEL_COUNT; i++)
				if (c[i] != 0.f)
				{
					n++;
					if (last)
						*last = i;
				}
			return n;
		}
		bool IsNumber() const
		{
			for (int i = 1; i < CHANNEL_COUNT; i++)
				if (c[i] != 0.f)
					return false;
			return true;
		}
	};

	struct UnitInfo {
		const char* name;
		Channel channel;
		float scale;
	};

	// Longest names first where one is a prefix of another.
	const UnitInfo units[] = {
		{"vmin", VMIN, 1.f},
		{"vmax", VMAX, 1.f},
		{"rem", REM, 1.f},
		{"deg", DEG, 1.f},
		{"rad", DEG, 57.29577951f},
		{"grad", DEG, 0.9f},
		{"turn", DEG, 360.f},
		{"px", PX, 1.f},
		{"em", EM, 1.f},
		{"ex", EM, 0.5f},
		{"ch", EM, 0.5f},
		{"vw", VW, 1.f},
		{"vh", VH, 1.f},
		{"dp", DP, 1.f},
		{"in", DP, 96.f},
		{"cm", DP, 96.f / 2.54f},
		{"mm", DP, 96.f / 25.4f},
		{"pt", DP, 96.f / 72.f},
		{"pc", DP, 96.f / 6.f},
		{"ms", SEC, 0.001f},
		{"s", SEC, 1.f},
		{"%", PERCENT, 1.f},
	};

	bool IsIdentChar(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
	}

	// Recursive descent parser and evaluator. With a context, per-element units are converted to pixels as they are read, so the
	// result only has NUMBER, PX, PERCENT, DEG and SEC parts and min()/max() can always decide.
	class Parser {
	public:
		Parser(const char* begin, const char* end, const Context* context) : p(begin), end(end), context(context) {}

		// Parses a whole math function call, e.g. "calc(...)". 'unresolved' is set if min()/max() could not decide without a context.
		bool ParseFunctionCall(Value& out)
		{
			SkipSpace();
			if (!ParseFunction(out))
				return false;
			SkipSpace();
			return p == end;
		}

		bool unresolved = false;

	private:
		const char* p;
		const char* end;
		const Context* context;

		void SkipSpace()
		{
			while (p < end && StringUtilities::IsWhitespace(*p))
				p++;
		}

		bool Accept(char c)
		{
			SkipSpace();
			if (p < end && *p == c)
			{
				p++;
				return true;
			}
			return false;
		}

		bool StartsWithWord(const char* word)
		{
			const size_t n = strlen(word);
			if ((size_t)(end - p) < n)
				return false;
			for (size_t i = 0; i < n; i++)
				if (std::tolower((unsigned char)p[i]) != word[i])
					return false;
			return true;
		}

		bool ParseFunction(Value& out)
		{
			static const char* names[] = {"calc(", "min(", "max(", "clamp("};
			int which = -1;
			for (int i = 0; i < 4; i++)
				if (StartsWithWord(names[i]))
				{
					which = i;
					p += strlen(names[i]);
					break;
				}
			if (which < 0)
				return false;

			Vector<Value> args;
			do
			{
				Value v;
				if (!ParseSum(v))
					return false;
				args.push_back(v);
			} while (Accept(','));
			if (!Accept(')'))
				return false;

			switch (which)
			{
			case 0:
				if (args.size() != 1)
					return false;
				out = args[0];
				return true;
			case 1:
			case 2:
				return Choose(args, which == 1, out);
			case 3:
			{
				if (args.size() != 3)
					return false;
				// clamp(MIN, VAL, MAX) = max(MIN, min(VAL, MAX))
				Value inner;
				Vector<Value> a = {args[1], args[2]};
				if (!Choose(a, true, inner))
					return false;
				Vector<Value> b = {args[0], inner};
				return Choose(b, false, out);
			}
			}
			return false;
		}

		// Comparable magnitude of a value, or false if it can't be compared without knowing the element.
		bool Magnitude(const Value& v, int shape, float& out) const
		{
			if (context)
			{
				const float base = (context->percent_base_guess >= 0.f ? context->percent_base_guess : context->viewport.x);
				out = v.c[NUMBER] + v.c[PX] + v.c[PERCENT] * base * 0.01f + v.c[DEG] + v.c[SEC];
				return true;
			}
			int last = -1;
			const int n = v.CountNonZero(&last);
			if (n == 0)
			{
				out = 0.f;
				return true;
			}
			if (n == 1 && (shape < 0 || last == shape))
			{
				out = v.c[last];
				return true;
			}
			return false;
		}

		bool Choose(const Vector<Value>& args, bool pick_min, Value& out)
		{
			if (args.empty())
				return false;

			// Without a context, all arguments must share one unit to be comparable.
			int shape = -1;
			if (!context)
				for (const Value& v : args)
				{
					int last = -1;
					if (v.CountNonZero(&last) == 1)
					{
						if (shape >= 0 && shape != last)
						{
							unresolved = true;
							out = args[0];
							return true;
						}
						shape = last;
					}
				}

			float best = 0.f;
			for (size_t i = 0; i < args.size(); i++)
			{
				float m = 0.f;
				if (!Magnitude(args[i], shape, m))
				{
					unresolved = true;
					out = args[0];
					return true;
				}
				if (i == 0 || (pick_min ? m < best : m > best))
				{
					best = m;
					out = args[i];
				}
			}
			return true;
		}

		bool ParseSum(Value& out)
		{
			if (!ParseProduct(out))
				return false;
			for (;;)
			{
				SkipSpace();
				if (p < end && (*p == '+' || *p == '-'))
				{
					const bool minus = (*p == '-');
					p++;
					Value rhs;
					if (!ParseProduct(rhs))
						return false;
					for (int i = 0; i < CHANNEL_COUNT; i++)
						out.c[i] += (minus ? -rhs.c[i] : rhs.c[i]);
				}
				else
					return true;
			}
		}

		bool ParseProduct(Value& out)
		{
			if (!ParseFactor(out))
				return false;
			for (;;)
			{
				SkipSpace();
				if (p < end && (*p == '*' || *p == '/'))
				{
					const bool divide = (*p == '/');
					p++;
					Value rhs;
					if (!ParseFactor(rhs))
						return false;
					if (divide)
					{
						if (!rhs.IsNumber() || rhs.c[NUMBER] == 0.f)
							return false;
						for (float& f : out.c)
							f /= rhs.c[NUMBER];
					}
					else if (rhs.IsNumber())
					{
						for (float& f : out.c)
							f *= rhs.c[NUMBER];
					}
					else if (out.IsNumber())
					{
						const float k = out.c[NUMBER];
						out = rhs;
						for (float& f : out.c)
							f *= k;
					}
					else
						return false;
				}
				else
					return true;
			}
		}

		bool ParseFactor(Value& out)
		{
			SkipSpace();
			if (p >= end)
				return false;

			if (*p == '(')
			{
				p++;
				if (!ParseSum(out))
					return false;
				return Accept(')');
			}

			const char c = *p;
			const bool numeric_start = (c >= '0' && c <= '9') || c == '.' ||
				((c == '-' || c == '+') && p + 1 < end && ((p[1] >= '0' && p[1] <= '9') || p[1] == '.'));
			if (numeric_start)
				return ParseNumber(out);

			if (c == '-' && p + 1 < end && std::isalpha((unsigned char)p[1]))
			{
				// A negated function or constant, e.g. -min(...) is not valid CSS but -pi is not either; be lenient.
				p++;
				if (!ParseFactor(out))
					return false;
				for (float& f : out.c)
					f = -f;
				return true;
			}

			if (StartsWithWord("pi") && (p + 2 >= end || !IsIdentChar(p[2])))
			{
				p += 2;
				out = Value();
				out.c[NUMBER] = 3.14159265f;
				return true;
			}
			if (StartsWithWord("e") && (p + 1 >= end || !IsIdentChar(p[1])))
			{
				p += 1;
				out = Value();
				out.c[NUMBER] = 2.71828183f;
				return true;
			}

			return ParseFunction(out);
		}

		bool ParseNumber(Value& out)
		{
			char* number_end = nullptr;
			const String text(p, end);
			const float number = strtof(text.c_str(), &number_end);
			if (number_end == text.c_str())
				return false;
			p += (number_end - text.c_str());

			out = Value();
			Channel channel = NUMBER;
			float scale = 1.f;
			for (const UnitInfo& unit : units)
			{
				const size_t n = strlen(unit.name);
				if ((size_t)(end - p) >= n && StartsWithWord(unit.name) && (p + n >= end || !IsIdentChar(p[n]) || unit.name[0] == '%'))
				{
					channel = unit.channel;
					scale = unit.scale;
					p += n;
					break;
				}
			}
			if (p < end && IsIdentChar(*p) && !(*p == '-'))
				return false; // unknown unit

			const float v = number * scale;
			if (!context)
			{
				out.c[channel] = v;
				return true;
			}

			switch (channel)
			{
			case EM: out.c[PX] = v * context->font_size; break;
			case REM: out.c[PX] = v * context->root_font_size; break;
			case VW: out.c[PX] = v * context->viewport.x * 0.01f; break;
			case VH: out.c[PX] = v * context->viewport.y * 0.01f; break;
			case VMIN: out.c[PX] = v * std::min(context->viewport.x, context->viewport.y) * 0.01f; break;
			case VMAX: out.c[PX] = v * std::max(context->viewport.x, context->viewport.y) * 0.01f; break;
			case DP: out.c[PX] = v * context->dp_ratio; break;
			default: out.c[channel] = v; break;
			}
			return true;
		}
	};

	// Finds the math function calls in a value; returns false for an unterminated call.
	template <typename Callback>
	bool ForEachMathCall(const String& value, Callback&& callback)
	{
		static const char* names[] = {"calc(", "min(", "max(", "clamp("};
		char quote = 0;
		for (size_t i = 0; i < value.size(); i++)
		{
			const char c = value[i];
			if (quote)
			{
				if (c == quote)
					quote = 0;
				continue;
			}
			if (c == '"' || c == '\'')
			{
				quote = c;
				continue;
			}
			if (i > 0 && IsIdentChar(value[i - 1]))
				continue;

			for (const char* name : names)
			{
				const size_t n = strlen(name);
				if (value.size() - i < n)
					continue;
				bool match = true;
				for (size_t k = 0; k < n && match; k++)
					match = (std::tolower((unsigned char)value[i + k]) == name[k]);
				if (!match)
					continue;

				int depth = 0;
				size_t j = i + n - 1;
				for (; j < value.size(); j++)
				{
					if (value[j] == '(')
						depth++;
					else if (value[j] == ')' && --depth == 0)
						break;
				}
				if (j >= value.size())
					return false;
				i = callback(i, j + 1);
				break;
			}
		}
		return true;
	}

	String FormatNumber(float value)
	{
		if (std::fabs(value) < 1e-6f)
			value = 0.f;
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%.6g", value);
		return String(buffer);
	}

} // namespace

bool IsCssWideKeyword(const String& value)
{
	return value == "inherit" || value == "initial" || value == "unset" || value == "revert" || value == "revert-layer";
}

bool ContainsMath(const String& value)
{
	bool found = false;
	ForEachMathCall(value, [&](size_t begin, size_t end) {
		found = true;
		(void)begin;
		return end - 1;
	});
	return found;
}

bool Fold(const String& value, String& out_value, bool& out_unresolved)
{
	static const char* suffix[CHANNEL_COUNT] = {"", "px", "%", "em", "rem", "vw", "vh", nullptr, nullptr, "dp", "deg", "s"};

	out_value.clear();
	out_unresolved = false;
	bool valid = true;
	size_t copied = 0;

	const bool terminated = ForEachMathCall(value, [&](size_t begin, size_t end) {
		Parser parser(value.data() + begin, value.data() + end, nullptr);
		Value v;
		if (!parser.ParseFunctionCall(v))
		{
			valid = false;
			return end - 1;
		}

		int channel = NUMBER;
		const int count = v.CountNonZero(&channel);
		if (parser.unresolved || count > 1 || !suffix[channel])
		{
			out_unresolved = true;
			return end - 1;
		}

		out_value.append(value, copied, begin - copied);
		out_value += FormatNumber(count == 0 ? 0.f : v.c[channel]);
		if (count == 1)
			out_value += suffix[channel];
		copied = end;
		return end - 1;
	});

	out_value.append(value, copied, String::npos);
	return terminated && valid;
}

Result Resolve(const String& expression, const Context& context)
{
	Result result;
	Parser parser(expression.data(), expression.data() + expression.size(), &context);
	Value v;
	if (!parser.ParseFunctionCall(v))
		return result;

	result.px = v.c[PX];
	result.percent = v.c[PERCENT];
	result.number = v.c[NUMBER];
	result.valid = true;
	return result;
}

} // namespace Calc
} // namespace Rml
