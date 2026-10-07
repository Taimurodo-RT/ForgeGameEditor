#pragma once

#include "Header.h"
#include "Types.h"

namespace Rml {

/*
    Forge: keeps CSS data URIs (url("data:image/svg+xml,...")) behind short keys, so they can travel through decorator and
    attribute values that cannot hold their quotes, commas and parentheses. The renderer resolves the key when loading the texture.
*/
namespace DataUri {

	/// The prefix of keys returned by Register().
	constexpr const char* KeyPrefix = "forge-data:";

	/// Stores a data URI and returns its key, the same key for the same URI.
	RMLUICORE_API String Register(const String& uri);

	/// Finds the data URI of a key. Returns false for unknown keys.
	RMLUICORE_API bool Lookup(const String& key, String& out_uri);

	/// Decodes a data URI into its media type and bytes (percent-encoded or base64). Returns false if it is not a data URI.
	RMLUICORE_API bool Decode(const String& uri, String& out_media_type, String& out_bytes);

} // namespace DataUri
} // namespace Rml
