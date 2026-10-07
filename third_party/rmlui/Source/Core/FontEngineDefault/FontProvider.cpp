#include "FontProvider.h"
#include "FontFaceHandleDefault.h"
#include "../../../Include/RmlUi/Core/Core.h"
#include "../../../Include/RmlUi/Core/FileInterface.h"
#include "../../../Include/RmlUi/Core/Log.h"
#include "../../../Include/RmlUi/Core/Math.h"
#include "../../../Include/RmlUi/Core/StringUtilities.h"
#include "../ComputeProperty.h"
#include "FontFace.h"
#include "FontFamily.h"
#include "FreeTypeInterface.h"
#include <algorithm>

namespace Rml {

static FontProvider* g_font_provider = nullptr;

FontProvider::FontProvider()
{
	RMLUI_ASSERT(!g_font_provider);
}

FontProvider::~FontProvider()
{
	RMLUI_ASSERT(g_font_provider == this);
}

bool FontProvider::Initialise()
{
	RMLUI_ASSERT(!g_font_provider);
	if (!FreeType::Initialise())
		return false;
	g_font_provider = new FontProvider;
	return true;
}

void FontProvider::Shutdown()
{
	RMLUI_ASSERT(g_font_provider);
	delete g_font_provider;
	g_font_provider = nullptr;
	FreeType::Shutdown();
}

FontProvider& FontProvider::Get()
{
	RMLUI_ASSERT(g_font_provider);
	return *g_font_provider;
}

FontFaceHandleDefault* FontProvider::GetFontFaceHandle(const String& family, Style::FontStyle style, Style::FontWeight weight, int size)
{
	RMLUI_ASSERTMSG(family == StringUtilities::ToLower(family), "Font family name must be converted to lowercase before entering here.");

	// Forge: a CSS font-family list, "Inter, \"Segoe UI\", sans-serif": the first family that is loaded, or one
	// the list names by an alias, else the default family.
	FontProvider& provider = Get();
	if (FontFamily* found = provider.FindFamily(family))
		return provider.WithWordGlyphs(found->GetFaceHandle(style, weight, size), family);
	if (family.find(',') == String::npos && family.find('"') == String::npos && family.find('\'') == String::npos &&
		provider.family_aliases.empty())
		return nullptr;

	StringList names;
	StringUtilities::ExpandString(names, family, ',', '"', '"', true);
	for (String& name : names)
	{
		name = StringUtilities::StripWhitespace(name);
		if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front())
			name = name.substr(1, name.size() - 2);
		if (FontFamily* found = provider.FindFamily(name))
			return provider.WithWordGlyphs(found->GetFaceHandle(style, weight, size), name);
		auto alias = provider.family_aliases.find(name);
		if (alias != provider.family_aliases.end())
			if (FontFamily* found = provider.FindFamily(alias->second))
				return provider.WithWordGlyphs(found->GetFaceHandle(style, weight, size), alias->second);
	}
	auto fallback = provider.family_aliases.find(String());
	if (fallback != provider.family_aliases.end())
		if (FontFamily* found = provider.FindFamily(fallback->second))
			return provider.WithWordGlyphs(found->GetFaceHandle(style, weight, size), fallback->second);
	return nullptr;
}

FontFaceHandleDefault* FontProvider::WithWordGlyphs(FontFaceHandleDefault* handle, const String& family)
{
	if (handle && !family_word_glyphs.empty())
	{
		auto it = family_word_glyphs.find(family);
		handle->SetWordGlyphs(it == family_word_glyphs.end() ? nullptr : it->second);
	}
	return handle;
}

void FontProvider::SetFamilyWordGlyphs(const String& family, UnorderedMap<String, Character> words)
{
	Get().family_word_glyphs[StringUtilities::ToLower(family)] = MakeShared<const UnorderedMap<String, Character>>(std::move(words));
}

FontFamily* FontProvider::FindFamily(const String& name)
{
	auto it = font_families.find(name);
	return it == font_families.end() ? nullptr : it->second.get();
}

void FontProvider::SetFamilyAlias(const String& generic, const String& family)
{
	Get().family_aliases[StringUtilities::ToLower(generic)] = StringUtilities::ToLower(family);
}

int FontProvider::CountFallbackFontFaces()
{
	return (int)Get().fallback_font_faces.size();
}

FontFaceHandleDefault* FontProvider::GetFallbackFontFace(int index, int font_size)
{
	auto& faces = FontProvider::Get().fallback_font_faces;

	if (index >= 0 && index < (int)faces.size())
		return faces[index]->GetHandle(font_size, false);

	return nullptr;
}

void FontProvider::ReleaseFontResources()
{
	RMLUI_ASSERT(g_font_provider);
	for (auto& name_family : g_font_provider->font_families)
		name_family.second->ReleaseFontResources();
}

bool FontProvider::LoadFontFace(const String& file_name, int face_index, bool fallback_face, Style::FontWeight weight)
{
	return LoadFontFace(file_name, face_index, {}, Style::FontStyle::Normal, weight, fallback_face);
}

bool FontProvider::LoadFontFace(const String& file_name, int face_index, const String& font_family, Style::FontStyle style, Style::FontWeight weight,
	bool fallback_face)
{
	FileInterface* file_interface = GetFileInterface();
	FileHandle handle = file_interface->Open(file_name);

	if (!handle)
	{
		Log::Message(Log::LT_ERROR, "Failed to load font face from %s, could not open file.", file_name.c_str());
		return false;
	}

	size_t length = file_interface->Length(handle);

	auto buffer_ptr = UniquePtr<byte[]>(new byte[length]);
	byte* buffer = buffer_ptr.get();
	file_interface->Read(buffer, length, handle);
	file_interface->Close(handle);

	bool result = Get().LoadFontFace({buffer, length}, face_index, fallback_face, std::move(buffer_ptr), file_name, font_family, style, weight);

	return result;
}

bool FontProvider::LoadFontFace(Span<const byte> data, int face_index, const String& font_family, Style::FontStyle style, Style::FontWeight weight,
	bool fallback_face)
{
	const String source = "memory";

	bool result = Get().LoadFontFace(data, face_index, fallback_face, nullptr, source, font_family, style, weight);

	return result;
}

bool FontProvider::LoadFontFace(Span<const byte> data, int face_index, bool fallback_face, UniquePtr<byte[]> face_memory, const String& source,
	String font_family, Style::FontStyle style, Style::FontWeight weight)
{
	using Style::FontWeight;

	Vector<FaceVariation> face_variations;
	if (!FreeType::GetFaceVariations(data, face_variations, face_index))
	{
		Log::Message(Log::LT_ERROR, "Failed to load font face from '%s': Invalid or unsupported font face file format.", source.c_str());
		return false;
	}

	Vector<FaceVariation> load_variations;
	if (face_variations.empty())
	{
		load_variations.push_back(FaceVariation{Style::FontWeight::Auto, 0, 0});
	}
	else
	{
		// Iterate through all the face variations and pick the ones to load. The list is already sorted by (weight, width). When weight is set to
		// 'auto' we load all the weights of the face. However, we only want to load one width for each weight.
		for (auto it = face_variations.begin(); it != face_variations.end();)
		{
			if (weight != FontWeight::Auto && it->weight != weight)
			{
				++it;
				continue;
			}

			// We don't currently have any way for users to select widths, so we search for a regular (medium) value here.
			constexpr int search_width = 100;
			const FontWeight current_weight = it->weight;

			int best_width_distance = Math::Absolute((int)it->width - search_width);
			auto it_best_width = it;

			// Search forward to find the best 'width' with the same weight.
			for (++it; it != face_variations.end(); ++it)
			{
				if (it->weight != current_weight)
					break;

				const int width_distance = Math::Absolute((int)it->width - search_width);
				if (width_distance < best_width_distance)
				{
					best_width_distance = width_distance;
					it_best_width = it;
				}
			}

			load_variations.push_back(*it_best_width);
		}
	}

	if (load_variations.empty())
	{
		Log::Message(Log::LT_ERROR, "Failed to load font face from '%s': Could not locate face with weight %d.", source.c_str(), (int)weight);
		return false;
	}

	for (const FaceVariation& variation : load_variations)
	{
		FontFaceHandleFreetype ft_face = FreeType::LoadFace(data, source, face_index, variation.named_instance_index);
		if (!ft_face)
			return false;

		if (font_family.empty())
			FreeType::GetFaceStyle(ft_face, &font_family, &style, nullptr);
		if (weight == FontWeight::Auto)
			FreeType::GetFaceStyle(ft_face, nullptr, nullptr, &weight);

		const FontWeight variation_weight = (variation.weight == FontWeight::Auto ? weight : variation.weight);
		const String font_face_description = GetFontFaceDescription(font_family, style, variation_weight);

		const FontFaceLoadResult result = AddFace(ft_face, font_family, style, variation_weight, fallback_face, std::move(face_memory));
		switch (result)
		{
		case FontFaceLoadResult::Success:
			Log::Message(Log::LT_INFO, "Loaded font face %s from '%s'.", font_face_description.c_str(), source.c_str());
			break;
		case FontFaceLoadResult::Duplicate:
			Log::Message(Log::LT_INFO, "Font face %s from '%s' already loaded, proceeding.", font_face_description.c_str(), source.c_str());
			break;
		case FontFaceLoadResult::Error:
			Log::Message(Log::LT_ERROR, "Failed to load font face %s from '%s'.", font_face_description.c_str(), source.c_str());
			return false;
		}
	}

	return true;
}

auto FontProvider::AddFace(FontFaceHandleFreetype face, const String& family, Style::FontStyle style, Style::FontWeight weight, bool fallback_face,
	UniquePtr<byte[]> face_memory) -> FontFaceLoadResult
{
	if (family.empty() || weight == Style::FontWeight::Auto)
		return FontFaceLoadResult::Error;

	String family_lower = StringUtilities::ToLower(family);
	FontFamily* font_family = nullptr;
	auto it = font_families.find(family_lower);
	if (it != font_families.end())
	{
		font_family = (FontFamily*)it->second.get();
	}
	else
	{
		auto font_family_ptr = MakeUnique<FontFamily>(family_lower);
		font_family = font_family_ptr.get();
		font_families[family_lower] = std::move(font_family_ptr);
	}

	const auto [result, face_ptr] = font_family->AddFace(face, style, weight, std::move(face_memory));
	if (result != FontFaceLoadResult::Success)
		return result;

	if (face_ptr && fallback_face)
	{
		auto it_fallback_face = std::find(fallback_font_faces.begin(), fallback_font_faces.end(), face_ptr);
		if (it_fallback_face == fallback_font_faces.end())
		{
			fallback_font_faces.push_back(face_ptr);
		}
	}

	return result;
}

} // namespace Rml
