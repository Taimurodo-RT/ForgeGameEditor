#include "FontFace.h"
#include "../../../Include/RmlUi/Core/Log.h"
#include "FontFaceHandleDefault.h"
#include "FreeTypeInterface.h"

namespace Rml {

FontFace::FontFace(FontFaceHandleFreetype _face, Style::FontStyle _style, Style::FontWeight _weight)
{
	style = _style;
	weight = _weight;
	face = _face;
}

FontFace::~FontFace()
{
	if (face)
		FreeType::ReleaseFace(face);
}

Style::FontStyle FontFace::GetStyle() const
{
	return style;
}

Style::FontWeight FontFace::GetWeight() const
{
	return weight;
}

FontFaceHandleDefault* FontFace::GetHandle(int size, bool load_default_glyphs, bool oblique)
{
	// Forge: synthesized italic handles are stored under the negated size.
	const int key = (oblique ? -size : size);
	auto it = handles.find(key);
	if (it != handles.end())
		return it->second.get();

	// See if this face has been released.
	if (!face)
	{
		Log::Message(Log::LT_WARNING, "Font face has been released, unable to generate new handle.");
		return nullptr;
	}

	// Construct and initialise the new handle.
	auto handle = MakeUnique<FontFaceHandleDefault>();
	if (!handle->Initialize(face, size, load_default_glyphs, oblique))
	{
		handles[key] = nullptr;
		return nullptr;
	}

	FontFaceHandleDefault* result = handle.get();

	// Save the new handle to the font face
	handles[key] = std::move(handle);

	return result;
}

void FontFace::ReleaseFontResources()
{
	HandleMap().swap(handles);
}

} // namespace Rml
