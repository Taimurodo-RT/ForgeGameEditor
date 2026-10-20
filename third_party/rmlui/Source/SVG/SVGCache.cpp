#include "SVGCache.h"
#include "../../Include/RmlUi/Core/CallbackTexture.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/DataUri.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/FileInterface.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "../../Include/RmlUi/Core/Texture.h"
#include "../../Include/RmlUi/Core/Utilities.h"
#include "../Core/ControlledLifetimeResource.h"
#include <algorithm>
#include <cstdio>
#include <lunasvg.h>
#include <string>

#ifdef RMLUI_SVG_DEBUG
	#define RMLUI_SVG_DEBUG_LOG(...) Rml::Log::Message(Rml::Log::LT_DEBUG, __VA_ARGS__)
#else
	#define RMLUI_SVG_DEBUG_LOG(...)
#endif

namespace Rml {
namespace SVG {
	struct SVGKey {
		String source_id;
		Vector2i dimensions;
		bool crop_to_content;
		ColourbPremultiplied colour;
		// Forge: the element's 'color' for pictures that use currentColor; transparent black for the others, so they share
		// one texture whatever the colour of the text around them.
		Colourb current_color;
		// Forge: the size the picture is drawn at, in window pixels before transforms; its raster ('dimensions') is made at
		// the scale of the transforms over it (see PictureScale).
		Vector2i quad;

		friend bool operator==(const SVGKey& lhs, const SVGKey& rhs)
		{
			return lhs.source_id == rhs.source_id && lhs.dimensions == rhs.dimensions && lhs.crop_to_content == rhs.crop_to_content &&
				lhs.colour == rhs.colour && lhs.current_color == rhs.current_color && lhs.quad == rhs.quad;
		}
	};
} // namespace SVG
} // namespace Rml

namespace std {
template <>
struct hash<::Rml::SVG::SVGKey> {
	size_t operator()(const ::Rml::SVG::SVGKey& key) const noexcept
	{
		size_t hash = 0;
		Rml::Utilities::HashCombine(hash, key.source_id);
		Rml::Utilities::HashCombine(hash, key.dimensions.x);
		Rml::Utilities::HashCombine(hash, key.dimensions.y);
		Rml::Utilities::HashCombine(hash, key.crop_to_content);
		static_assert(sizeof(uint32_t) == sizeof(key.colour), "Expecting color to be 4 bytes");
		Rml::Utilities::HashCombine(hash, *reinterpret_cast<const uint32_t*>(&key.colour[0]));
		Rml::Utilities::HashCombine(hash, *reinterpret_cast<const uint32_t*>(&key.current_color[0])); // Forge
		Rml::Utilities::HashCombine(hash, key.quad.x);                                                 // Forge
		Rml::Utilities::HashCombine(hash, key.quad.y);                                                 // Forge
		return hash;
	}
};
} // namespace std

namespace Rml {
namespace SVG {

	static SharedPtr<SVGData> GetHandle(RenderManager& render_manager, String source_id, const String& source, SVGCache::SourceType source_type,
		Vector2i dimensions, Vector2i quad, bool crop_to_content, ColourbPremultiplied colour, Colourb current_color, Element* live_element);
	static void ReleaseHandle(SVGData* handle);

	struct SVGGeometry {
		ColourbPremultiplied colour;
		Vector2i quad; // Forge: see SVGKey::quad
		UniquePtr<Geometry> geometry;
	};

	struct SVGTexture {
		Vector2i render_dimensions;
		bool crop_to_content;
		Colourb current_color; // Forge: see SVGKey::current_color
		CallbackTexture texture;
		// List of geometries using this texture, one entry for each unique color.
		Vector<SVGGeometry> geometries;
	};

	struct SVGDocument {
		Vector2f intrinsic_dimensions;
		UniquePtr<lunasvg::Document> svg_document;
		// Forge: the picture uses currentColor, so its textures differ by the element's 'color'.
		bool uses_current_color = false;
		// Forge: a live element's own document: the file (or inline key) and the text it was read from; the document its
		// apply draws over it, at its scale.
		String live_path, live_text;
		UniquePtr<lunasvg::Document> live_over;
		// List of textures using this document, one entry for each unique render dimension plus meta-data.
		Vector<SVGTexture> textures;
	};

	struct SVGCacheData {
		// A list of SVG documents mapped by their path. Owns all geometry and textures needed for rendering.
		UnorderedMap<String, SVGDocument> documents;

		// Handles are reference-counted lookup keys and views into the SVG document resources. Handles are responsible
		// for cleaning up the resources in the documents, when nothing refers to them any longer. When a handle is
		// destroyed, it also removes itself from the handle map.
		StableUnorderedMap<SVGKey, WeakPtr<SVGData>> handles;

		// Forge: the live elements (see SVGCache::SetLive), each with the start of its documents' keys.
		struct LiveElement {
			ObserverPtr<Element> element;
			String prefix;
		};
		UnorderedMap<Element*, LiveElement> live;
		unsigned int live_counter = 0;
	};

	static ControlledLifetimeResource<SVGCacheData> svg_cache_data;
	static LiveApply live_apply; // Forge

	// Forge: the key of a picture for this element: its own document's when it is live, else the shared one's.
	static String LiveId(Element* element, const String& id, Element*& live_element)
	{
		live_element = nullptr;
		auto& live = svg_cache_data->live;
		if (live.empty() || !element)
			return id;
		auto it = live.find(element);
		if (it == live.end())
			return id;
		if (!it->second.element)
		{
			live.erase(it); // another element now at a dead one's address
			return id;
		}
		live_element = element;
		return it->second.prefix + id;
	}

	// Forge: the size of a raster for a picture drawn at 'dimensions', made at a scale: no side longer than 4096 pixels (a
	// bigger picture is stretched from it), none shorter than 1.
	static Vector2i RasterSize(Vector2i dimensions, float scale)
	{
		if (dimensions.x <= 0 || dimensions.y <= 0)
			return {0, 0};
		Vector2f raster = Vector2f(dimensions) * scale;
		const int max_side = 4096;
		if (raster.x > max_side || raster.y > max_side)
			raster *= float(max_side) / Math::Max(raster.x, raster.y);
		return Math::Max(Vector2i(raster.Round()), Vector2i(1));
	}

	SVGData::SVGData(Geometry& geometry, Texture texture, Vector2f intrinsic_dimensions, const SVGKey& cache_key) :
		geometry(geometry), texture(texture), intrinsic_dimensions(intrinsic_dimensions), cache_key(cache_key)
	{}

	SVGData::~SVGData()
	{
		ReleaseHandle(this);
	}

	static Vector<SVGTexture>::iterator FindSVGTexture(SVGDocument& doc, Vector2i dimensions, bool crop_to_content, Colourb current_color)
	{
		return std::find_if(doc.textures.begin(), doc.textures.end(), [&](const auto& entry) {
			return entry.render_dimensions == dimensions && entry.crop_to_content == crop_to_content && entry.current_color == current_color;
		});
	}

	// Forge: the text of a picture from a file, a data URI or the key of one (see DataUri).
	static bool LoadSource(const String& path, String& out_data)
	{
		String uri;
		if (!DataUri::Lookup(path, uri) && StringUtilities::StartsWith(path, "data:"))
			uri = path;
		if (!uri.empty())
		{
			String media_type;
			return DataUri::Decode(uri, media_type, out_data);
		}
		return !path.empty() && GetFileInterface()->LoadFile(path, out_data);
	}

	static Vector<SVGGeometry>::iterator FindSVGGeometry(SVGTexture& per_size_data, const ColourbPremultiplied colour, const Vector2i quad)
	{
		return std::find_if(per_size_data.geometries.begin(), per_size_data.geometries.end(),
			[&](const SVGGeometry& data) { return data.colour == colour && data.quad == quad; }); // Forge: and the quad's size
	}

	static const String& GetSourceOr(const lunasvg::Document* svg_document, const String& default_value)
	{
		const auto& documents = svg_cache_data->documents;
		auto it = std::find_if(documents.begin(), documents.end(),
			[svg_document](const auto& pair) { return pair.second.svg_document.get() == svg_document; });
		if (it != documents.end())
			return it->first;
		return default_value;
	}

	static SharedPtr<SVGData> GetHandle(RenderManager& render_manager, String move_from_id, const String& source, SVGCache::SourceType source_type,
		const Vector2i dimensions, const Vector2i quad, const bool crop_to_content, const ColourbPremultiplied colour, const Colourb current_color,
		Element* live_element)
	{
		// Forge: the document is found (or read) before the handle, as only the document knows whether the key takes the
		// element's colour (currentColor).
		const String source_id = std::move(move_from_id);
		auto& documents = svg_cache_data->documents;
		auto& handles = svg_cache_data->handles;

		// Find or create a document
		auto it_svg_document = documents.find(source_id);
		if (it_svg_document == documents.cend())
		{
			SVGDocument doc;
			if (source_type == SVGCache::SourceType::File)
			{
				RMLUI_SVG_DEBUG_LOG("Loading SVG document from file %s", source.c_str());
				String svg_data;
				if (!LoadSource(source, svg_data)) // Forge: data URIs too
				{
					Log::Message(Rml::Log::Type::LT_WARNING, "Could not load SVG file %s", source.c_str());
					return {};
				}

				// We use a reset-release approach here in case clients use a non-std unique_ptr (lunasvg uses std::unique_ptr). We also use
				// loadFromData(char*, size_t) instead of loadFromData(std::string) in case clients use a non-std string.
				doc.svg_document.reset(lunasvg::Document::loadFromData(svg_data.data(), svg_data.size()).release());
				doc.uses_current_color = StringUtilities::ToLower(svg_data).find("currentcolor") != String::npos; // Forge
				if (live_element) // Forge
				{
					doc.live_path = source;
					doc.live_text = std::move(svg_data);
				}
			}
			else
			{
				RMLUI_SVG_DEBUG_LOG("Loading SVG document from element %s contents", source_id.c_str());
				// We use a reset-release approach here in case clients use a non-std unique_ptr (lunasvg uses std::unique_ptr). We also use
				// loadFromData(char*, size_t) instead of loadFromData(std::string) in case clients use a non-std string.
				doc.svg_document.reset(lunasvg::Document::loadFromData(source.data(), source.size()).release());
				doc.uses_current_color = StringUtilities::ToLower(source).find("currentcolor") != String::npos; // Forge
				if (live_element) // Forge
				{
					doc.live_path = source_id.substr(svg_cache_data->live[live_element].prefix.size());
					doc.live_text = source;
				}
			}

			if (!doc.svg_document)
			{
				Log::Message(Rml::Log::Type::LT_WARNING, "Could not load SVG data for item %s", source_id.c_str());
				return {};
			}

			// Forge: a live element's document starts as the element's script has it (maybe another one in its place).
			if (live_element && live_apply)
				live_apply(live_element, doc.live_path, doc.live_text, doc.svg_document, doc.live_over, true);
			if (!doc.svg_document)
				return {};

			doc.intrinsic_dimensions.x = Math::Max(float(doc.svg_document->width()), 1.0f);
			doc.intrinsic_dimensions.y = Math::Max(float(doc.svg_document->height()), 1.0f);

			const auto it_inserted = documents.insert_or_assign(source_id, std::move(doc));
			RMLUI_ASSERT(it_inserted.second);

			it_svg_document = it_inserted.first;
		}

		SVGDocument& doc = it_svg_document->second;

		// Forge: the key, with the element's colour only for pictures that use it.
		SVGKey key{source_id, dimensions, crop_to_content, colour, doc.uses_current_color ? current_color : Colourb(0, 0, 0, 0), quad};
		const auto it_handle = handles.find(key);
		if (it_handle != handles.cend())
		{
			RMLUI_SVG_DEBUG_LOG("Found handle, reusing: %s, (%d, %d), %s, %#x", source_id.c_str(), dimensions.x, dimensions.y,
				crop_to_content ? "crop_to_content" : "crop_none", *reinterpret_cast<const uint32_t*>(&colour[0]));
			SharedPtr<SVGData> result = it_handle->second.lock();
			RMLUI_ASSERTMSG(result, "Failed to lock handle in SVG cache");
			return result;
		}

		RMLUI_SVG_DEBUG_LOG("Making new handle: %s, (%d, %d), %s, %#x", source_id.c_str(), dimensions.x, dimensions.y,
			crop_to_content ? "crop_to_content" : "crop_none", *reinterpret_cast<const uint32_t*>(&colour[0]));

		Vector2f intrinsic_dimensions = doc.intrinsic_dimensions;
		if (crop_to_content)
		{
			const lunasvg::Box smallest_fit = doc.svg_document->boundingBox();
			intrinsic_dimensions.x = static_cast<float>(smallest_fit.w);
			intrinsic_dimensions.y = static_cast<float>(smallest_fit.h);
		}

		// Find or create texture
		auto it_size = FindSVGTexture(doc, dimensions, crop_to_content, key.current_color);
		if (it_size == doc.textures.cend())
		{
			RMLUI_SVG_DEBUG_LOG("Creating per-size data for (%d, %d), %s", dimensions.x, dimensions.y,
				crop_to_content ? "crop_to_content" : "crop_none");
			SVGTexture svg_texture;
			svg_texture.render_dimensions = dimensions;
			svg_texture.crop_to_content = crop_to_content;
			svg_texture.current_color = key.current_color;
			svg_texture.texture = {};

			// Callback for generating texture.
			// Forge: the document is found when the texture is made, as a live element's apply may put another in its place.
			auto texture_callback = [source_id, dimensions, crop_to_content, uses_current_color = doc.uses_current_color,
										current_color = key.current_color](const CallbackTextureInterface& texture_interface) -> bool {
				const auto it_doc = svg_cache_data->documents.find(source_id);
				if (it_doc == svg_cache_data->documents.end() || !it_doc->second.svg_document)
					return false;
				lunasvg::Document* svg_document = it_doc->second.svg_document.get();
				lunasvg::Document* over = it_doc->second.live_over.get(); // Forge
				RMLUI_SVG_DEBUG_LOG("Generating texture: %s, (%d, %d), %s", GetSourceOr(svg_document, "").c_str(), dimensions.x, dimensions.y,
					crop_to_content ? "crop_to_content" : "crop_none");

				if (dimensions.x == 0 || dimensions.y == 0)
					return false;

				// Forge: currentColor is the element's 'color', given to the picture's root (textures of one document share it).
				if (uses_current_color)
				{
					char value[64];
					std::snprintf(value, sizeof(value), "rgba(%d,%d,%d,%.4f)", current_color.red, current_color.green, current_color.blue,
						current_color.alpha / 255.0);
					svg_document->documentElement().setAttribute("color", value);
					if (over)
						over->documentElement().setAttribute("color", value);
				}

				lunasvg::Bitmap bitmap;
				if (crop_to_content)
				{
					const lunasvg::Box smallest_fit = svg_document->boundingBox();

					lunasvg::Matrix matrix(dimensions.x / svg_document->width(), 0, 0, dimensions.y / svg_document->height(), 0, 0);
					matrix.scale(svg_document->width() / smallest_fit.w, svg_document->height() / smallest_fit.h);
					matrix.translate(-smallest_fit.x, -smallest_fit.y);

					bitmap = lunasvg::Bitmap(dimensions.x, dimensions.y);
					bitmap.clear(0x00000000);
					svg_document->render(bitmap, matrix);
					if (over) // Forge
						over->render(bitmap, matrix);
				}
				else
				{
					bitmap = svg_document->renderToBitmap(dimensions.x, dimensions.y);
					// Forge: the second document over it, at its scale (renderToBitmap's).
					if (over && bitmap.valid() && svg_document->width() > 0 && svg_document->height() > 0)
						over->render(bitmap,
							lunasvg::Matrix(dimensions.x / svg_document->width(), 0, 0, dimensions.y / svg_document->height(), 0, 0));
				}

				if (!bitmap.valid() || !bitmap.data())
				{
					Log::Message(Rml::Log::Type::LT_WARNING, "Could not render SVG to bitmap: %s", GetSourceOr(svg_document, "").c_str());
					return false;
				}

				// Swap red and blue channels, assuming LunaSVG v2.3.2 or newer, to convert to RmlUi's expected RGBA-ordering.
				const size_t bitmap_byte_size = bitmap.width() * bitmap.height() * 4;
				uint8_t* bitmap_data = bitmap.data();
				for (size_t i = 0; i < bitmap_byte_size; i += 4)
					std::swap(bitmap_data[i], bitmap_data[i + 2]);

				if (!texture_interface.GenerateTexture({reinterpret_cast<const Rml::byte*>(bitmap.data()), bitmap_byte_size},
						Vector2i{bitmap.width(), bitmap.height()}))
				{
					Log::Message(Rml::Log::Type::LT_WARNING, "Could not generate texture for SVG: %s", GetSourceOr(svg_document, "").c_str());
					return false;
				}

				return true;
			};

			svg_texture.texture = render_manager.MakeCallbackTexture(std::move(texture_callback));

			doc.textures.push_back(std::move(svg_texture));
			it_size = std::prev(doc.textures.end());
		}

		// Construct and insert per-color geometry
		SVGTexture& size_data = *it_size;
		RMLUI_ASSERTMSG(FindSVGGeometry(size_data, colour, quad) == size_data.geometries.end(),
			"We found an existing color entry in the SVG document cache, this should have been found as a cache key map entry instead.");
		SVGGeometry colour_data;
		colour_data.colour = colour;
		colour_data.quad = quad; // Forge
		Mesh mesh;
		MeshUtilities::GenerateQuad(mesh, Vector2f(0), Vector2f(quad), colour, Vector2f(0), Vector2f(1)); // Forge: the quad, not the raster
		colour_data.geometry = MakeUnique<Geometry>(render_manager.MakeGeometry(std::move(mesh)));
		size_data.geometries.push_back(std::move(colour_data));

		// Create and insert the handle
		const auto iterator_inserted = handles.emplace(std::move(key), WeakPtr<SVGData>());
		RMLUI_ASSERTMSG(iterator_inserted.second, "Could not insert entry into the SVG cache handle map, duplicate key.");
		const SVGKey& inserted_key = iterator_inserted.first->first;
		WeakPtr<SVGData>& inserted_weak_data_pointer = iterator_inserted.first->second;

		auto svg_handle = MakeShared<SVGData>(*size_data.geometries.back().geometry.get(), size_data.texture, intrinsic_dimensions, inserted_key);
		inserted_weak_data_pointer = svg_handle;

		return svg_handle;
	}

	static void ReleaseHandle(SVGData* handle)
	{
		// There are no longer any users of the cache entry uniquely identified by the handle address. Start from the
		// tip (i.e. per-color data) and remove that entry from its parent. Move up the cache ancestry and erase any
		// entries that no longer have any children.
		auto& documents = svg_cache_data->documents;
		auto& handles = svg_cache_data->handles;
		const SVGKey& key = handle->cache_key;

		auto it_handle = handles.find(key);
		RMLUI_ASSERT(it_handle != handles.cend());

		const auto it_document = documents.find(key.source_id);
		RMLUI_ASSERT(it_document != documents.cend());
		SVGDocument& svg_document = it_document->second;

		RMLUI_SVG_DEBUG_LOG("Releasing handle: %s, (%d, %d), %s, %#x", key.path.c_str(), key.dimensions.x, key.dimensions.y,
			key.crop_to_content ? "crop_to_content" : "crop_none", *reinterpret_cast<const uint32_t*>(&key.colour[0]));

		auto it_texture = FindSVGTexture(svg_document, key.dimensions, key.crop_to_content, key.current_color);
		RMLUI_ASSERT(it_texture != svg_document.textures.cend());
		SVGTexture& svg_texture = *it_texture;

		auto it_geometry = FindSVGGeometry(svg_texture, key.colour, key.quad);
		RMLUI_ASSERT(it_geometry != svg_texture.geometries.cend());

		if (svg_texture.geometries.size() > 1)
		{
			RMLUI_SVG_DEBUG_LOG("Releasing handle from geometries, size: %zu", svg_texture.geometries.size());
			std::iter_swap(it_geometry, std::prev(svg_texture.geometries.end()));
			svg_texture.geometries.pop_back();
		}
		else if (svg_document.textures.size() > 1)
		{
			RMLUI_SVG_DEBUG_LOG("Releasing handle from textures, size: %zu", svg_document.textures.size());
			std::iter_swap(it_texture, std::prev(svg_document.textures.end()));
			svg_document.textures.pop_back();
		}
		else
		{
			RMLUI_SVG_DEBUG_LOG("Releasing document");
			documents.erase(it_document);
		}

		handles.erase(it_handle);

#ifdef RMLUI_DEBUG
		size_t count_unique_entries = 0;
		for (auto& document : documents)
		{
			RMLUI_ASSERT(!document.second.textures.empty());
			for (auto& size_data : document.second.textures)
			{
				RMLUI_ASSERT(!size_data.geometries.empty());
				count_unique_entries += size_data.geometries.size();
			}
		}
		RMLUI_ASSERT(count_unique_entries == handles.size());
#endif
	}

	void SVGCache::Initialize()
	{
		svg_cache_data.Initialize();
	}

	void SVGCache::Shutdown()
	{
		svg_cache_data.Shutdown();
	}

	SharedPtr<SVGData> SVGCache::GetHandle(const String& source_id, const String& source, SourceType source_type, Element* element,
		const bool crop_to_content, const BoxArea area, const float raster_scale)
	{
		RenderManager* render_manager = element->GetRenderManager();
		if (!render_manager)
			return {};

		const ComputedValues& computed = element->GetComputedValues();
		const ColourbPremultiplied colour = computed.image_color().ToPremultiplied(computed.opacity());
		const Colourb current_color = computed.color(); // Forge
		Vector2i dimensions(element->GetBox().GetSize(area).Round());
		if (dimensions.x == 0 || dimensions.y == 0)
			dimensions = {0, 0};
		// Forge: drawn at the box's size, made at the scale of the transforms over it.
		const Vector2i quad = dimensions;
		dimensions = RasterSize(dimensions, raster_scale);

		if (source_type == File)
		{
			String path;
			if (ElementDocument* document = element->GetOwnerDocument())
			{
				const String document_source_url = StringUtilities::Replace(document->GetSourceURL(), '|', ':');
				GetSystemInterface()->JoinPath(path, document_source_url, source_id);
			}
			Element* live_element = nullptr; // Forge
			String id = LiveId(element, path, live_element);
			return Rml::SVG::GetHandle(*render_manager, std::move(id), path, source_type, dimensions, quad, crop_to_content, colour, current_color,
				live_element);
		}

		Element* live_element = nullptr; // Forge
		String id = LiveId(element, source_id, live_element);
		return Rml::SVG::GetHandle(*render_manager, std::move(id), source, source_type, dimensions, quad, crop_to_content, colour, current_color,
			live_element);
	}

	SharedPtr<SVGData> SVGCache::GetHandle(const String& path, Element* element, Vector2i dimensions, const bool crop_to_content, Vector2i quad)
	{
		RenderManager* render_manager = element->GetRenderManager();
		if (!render_manager)
			return {};
		const ComputedValues& computed = element->GetComputedValues();
		const ColourbPremultiplied colour = computed.image_color().ToPremultiplied(computed.opacity());
		if (dimensions.x <= 0 || dimensions.y <= 0)
			dimensions = {0, 0};
		if (quad.x <= 0 || quad.y <= 0)
			quad = dimensions;
		dimensions = RasterSize(dimensions, 1.f);
		Element* live_element = nullptr; // Forge
		String id = LiveId(element, path, live_element);
		return Rml::SVG::GetHandle(*render_manager, std::move(id), path, File, dimensions, quad, crop_to_content, colour, computed.color(),
			live_element);
	}

	void SVGCache::SetLiveApply(LiveApply apply)
	{
		live_apply = std::move(apply);
	}

	void SVGCache::SetLive(Element* element, bool live)
	{
		if (!element)
			return;
		auto& elements = svg_cache_data->live;
		for (auto it = elements.begin(); it != elements.end();)
			it = it->second.element ? std::next(it) : elements.erase(it);
		const auto it = elements.find(element);
		if (live == (it != elements.end()))
			return;
		if (live)
			elements.emplace(element,
				SVGCacheData::LiveElement{element->GetObserverPtr(), "forge-live:" + std::to_string(svg_cache_data->live_counter++) + "|"});
		else
			elements.erase(it);
		element->DirtyPictures();
	}

	bool SVGCache::IsLive(Element* element)
	{
		const auto it = svg_cache_data->live.find(element);
		return it != svg_cache_data->live.end() && it->second.element;
	}

	void SVGCache::Touch(Element* element)
	{
		const auto it = svg_cache_data->live.find(element);
		if (it == svg_cache_data->live.end() || !it->second.element)
			return;
		const String& prefix = it->second.prefix;
		for (auto& entry : svg_cache_data->documents)
		{
			if (!StringUtilities::StartsWith(entry.first, prefix))
				continue;
			SVGDocument& doc = entry.second;
			if (live_apply && doc.svg_document)
				live_apply(element, doc.live_path, doc.live_text, doc.svg_document, doc.live_over, false);
			for (SVGTexture& texture : doc.textures)
				texture.texture.Regenerate();
		}
	}

	bool SVGCache::IsSVG(const String& path)
	{
		String lower = StringUtilities::ToLower(path);
		String uri;
		if (DataUri::Lookup(path, uri))
			lower = StringUtilities::ToLower(uri);
		if (StringUtilities::StartsWith(lower, "data:"))
			return StringUtilities::StartsWith(lower, "data:image/svg");
		const size_t query = lower.find_first_of("?#");
		if (query != String::npos)
			lower.resize(query);
		return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".svg") == 0;
	}

	// Forge: RmlUi/SVG/LivePicture.h
	void SetLiveApply(LiveApply apply)
	{
		SVGCache::SetLiveApply(std::move(apply));
	}
	void SetLive(Element* element, bool live)
	{
		SVGCache::SetLive(element, live);
	}
	bool IsLive(Element* element)
	{
		return SVGCache::IsLive(element);
	}
	void Touch(Element* element)
	{
		SVGCache::Touch(element);
	}

} // namespace SVG
} // namespace Rml
