#pragma once

#include "../../Include/RmlUi/Core/Texture.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {

class Element;
class Geometry;

namespace SVG {

	struct SVGKey;

	struct SVGData : NonCopyMoveable {
		SVGData(Geometry& geometry, Texture texture, Vector2f intrinsic_dimensions, const SVGKey& cache_key);
		~SVGData();

		Geometry& geometry;
		Texture texture;
		Vector2f intrinsic_dimensions;
		const SVGKey& cache_key;
	};

	class SVGCache {
	public:
		enum SourceType {
			File = 1, /// The source is a file path.
			Data = 2  /// The source is raw SVG data.
		};
		static void Initialize();
		static void Shutdown();

		/// Returns a handle to SVG data matching the parameters - creates new data if none is found.
		/// @param[in] source_id Key to be used for caching the SVG data.
		/// @param[in] source SVG source data or Path to a file containing the SVG source data (type specified by source_type).
		/// @param[in] source_type If source refers to a file or is the SVG source data itself.
		/// @param[in] element Element for which to calculate the dimensions and color.
		/// @param[in] crop_to_content Crop the rendered SVG to its contents.
		/// @param[in] area The area of the element used to determine the SVG dimensions.
		/// @return A handle to the SVG data, with automatic reference counting.
		///	@note When changing color or dimensions of an SVG without changing the source file, it's best to get a
		/// new handle before releasing the old one, to avoid unnecessarily reloading data.
		static SharedPtr<SVGData> GetHandle(const String& source_id, const String& source, SourceType source_type, Element* element,
			const bool crop_to_content, const BoxArea area);

		/// Forge: a handle for a picture drawn at a size the caller decides, in device pixels (CSS backgrounds and masks,
		/// <img>, border-image). 'path' is resolved already: a file, a data URI or a data URI key (see DataUri).
		static SharedPtr<SVGData> GetHandle(const String& path, Element* element, Vector2i dimensions, bool crop_to_content = false);

		/// Forge: true if the path names an SVG picture: a '.svg' file, an SVG data URI or the key of one.
		static bool IsSVG(const String& path);
	};

} // namespace SVG
} // namespace Rml
