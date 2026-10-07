#include "../../Include/RmlUi/Core/DataUri.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include <mutex>

namespace Rml {
namespace DataUri {

	namespace {
		struct Registry {
			std::mutex mutex;
			UnorderedMap<String, String> uris;
		};
		Registry& GetRegistry()
		{
			static Registry registry;
			return registry;
		}

		int HexValue(char c)
		{
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			return -1;
		}

		int Base64Value(char c)
		{
			if (c >= 'A' && c <= 'Z')
				return c - 'A';
			if (c >= 'a' && c <= 'z')
				return c - 'a' + 26;
			if (c >= '0' && c <= '9')
				return c - '0' + 52;
			if (c == '+' || c == '-')
				return 62;
			if (c == '/' || c == '_')
				return 63;
			return -1;
		}
	} // namespace

	String Register(const String& uri)
	{
		// FNV-1a, 64 bits.
		uint64_t hash = 14695981039346656037ull;
		for (char c : uri)
		{
			hash ^= (unsigned char)c;
			hash *= 1099511628211ull;
		}
		String key = KeyPrefix;
		static const char digits[] = "0123456789abcdef";
		for (int i = 15; i >= 0; i--)
			key += digits[(hash >> (i * 4)) & 0xF];

		Registry& registry = GetRegistry();
		std::lock_guard<std::mutex> lock(registry.mutex);
		registry.uris[key] = uri;
		return key;
	}

	bool Lookup(const String& key, String& out_uri)
	{
		Registry& registry = GetRegistry();
		std::lock_guard<std::mutex> lock(registry.mutex);
		auto it = registry.uris.find(key);
		if (it == registry.uris.end())
			return false;
		out_uri = it->second;
		return true;
	}

	bool Decode(const String& uri, String& out_media_type, String& out_bytes)
	{
		if (uri.size() < 5 || StringUtilities::ToLower(uri.substr(0, 5)) != "data:")
			return false;
		const size_t comma = uri.find(',');
		if (comma == String::npos)
			return false;
		String header = StringUtilities::ToLower(uri.substr(5, comma - 5));
		bool base64 = false;
		if (header.size() >= 7 && header.compare(header.size() - 7, 7, ";base64") == 0)
		{
			base64 = true;
			header.resize(header.size() - 7);
		}
		out_media_type = header.substr(0, header.find(';'));

		out_bytes.clear();
		const char* p = uri.data() + comma + 1;
		const char* end = uri.data() + uri.size();
		if (base64)
		{
			unsigned int buffer = 0;
			int bits = 0;
			for (; p < end; ++p)
			{
				if (*p == '%' && p + 2 < end && HexValue(p[1]) >= 0 && HexValue(p[2]) >= 0)
					p += 2; // Percent-encoded padding or whitespace.
				const int value = Base64Value(*p);
				if (value < 0)
					continue;
				buffer = (buffer << 6) | (unsigned int)value;
				bits += 6;
				if (bits >= 8)
				{
					bits -= 8;
					out_bytes += char((buffer >> bits) & 0xFF);
				}
			}
		}
		else
		{
			for (; p < end; ++p)
			{
				if (*p == '%' && p + 2 < end && HexValue(p[1]) >= 0 && HexValue(p[2]) >= 0)
				{
					out_bytes += char(HexValue(p[1]) * 16 + HexValue(p[2]));
					p += 2;
				}
				else
					out_bytes += *p;
			}
		}
		return true;
	}

} // namespace DataUri
} // namespace Rml
