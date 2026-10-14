#pragma once

// Forge: live SVG pictures (step 4 of docs/models/вектор-модель.md). A live element draws its SVG pictures (an <img>, a
// <svg>, a decorator, a mask image, a border-image) from documents of its own, read from the same files, so a script
// can change their parts; other elements showing a file keep it as it is. The apply function changes such a document:
// when it is made (from the file's text) and on each Touch; the element draws it again, at its size, when next used.

#include "../Core/Header.h"
#include "../Core/Types.h"

namespace lunasvg {
class Document;
}

namespace Rml {

class Element;

namespace SVG {

	/// 'path' is the picture's file (or the key of an inline <svg>), 'text' what the document was read from.
	using LiveApply = Function<void(Element* element, const String& path, const String& text, lunasvg::Document& document)>;
	RMLUICORE_API void SetLiveApply(LiveApply apply);
	/// Makes the element live, or takes it back to the shared pictures; it takes its pictures again either way.
	RMLUICORE_API void SetLive(Element* element, bool live);
	RMLUICORE_API bool IsLive(Element* element);
	/// The live element's documents are changed again by the apply function and drawn again when next used.
	RMLUICORE_API void Touch(Element* element);

} // namespace SVG
} // namespace Rml
