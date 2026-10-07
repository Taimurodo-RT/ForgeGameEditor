#include "forge/ui/ui.h"

#include "elements.h"
#include "gpu_renderer.h"
#include "interfaces.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"
#include "forge/ui/tokens.h"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <SDL3/SDL.h>
#include <yyjson.h>

#include <algorithm>

namespace forge::ui {

struct Ui::Impl {
    SDL_GPUDevice* device = nullptr;
    SDL_Window* window = nullptr;
    UiConfig config;
    GpuRenderer renderer;
    FileInterface files;
    SystemInterface system;
    Tokens tokens;
    bool initialized = false;

    struct Document {
        Rml::Context* context;
        std::string path;
        Rml::ElementDocument* document;
    };
    std::vector<Rml::Context*> contexts;
    std::vector<Document> documents;
    struct Offscreen {
        std::string image;
        SDL_GPUTexture* texture = nullptr;
        u32 width = 0, height = 0;
    };
    std::unordered_map<Rml::Context*, Offscreen> offscreen;
    std::unordered_map<Rml::Context*, bool> inactive;
    void release_offscreen(Offscreen& o);
    bool ensure_offscreen(Rml::Context* context, Offscreen& o);
    std::unique_ptr<Rml::ElementInstancer> icon_instancer;
    std::unique_ptr<Rml::ElementInstancer> list_instancer;
    std::unique_ptr<Rml::ElementInstancer> lines_instancer;
    std::unique_ptr<Rml::ElementInstancer> shape_instancer;
    bool debugger = false;

    // Hot reload.
    std::unordered_map<std::string, std::filesystem::file_time_type> known;
    std::filesystem::path tokens_path;
    std::filesystem::file_time_type tokens_time{};
    u64 next_poll_ns = 0;

    UiStats stats;
    std::function<void()> on_reload;

    bool load_fonts();
    void poll_files();
    void restyle();
    void reload();
};

Ui::Ui() : impl_(std::make_unique<Impl>()) {}
Ui::~Ui() { shutdown(); }

bool Ui::Impl::load_fonts() {
    std::vector<u8> bytes;
    const std::filesystem::path path = config.root / "fonts" / "fonts.json";
    if (!read_file(path, bytes)) {
        FORGE_ERROR("ui: cannot read %s", path_to_utf8(path).c_str());
        return false;
    }
    yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0);
    yyjson_val* fonts = yyjson_obj_get(yyjson_doc_get_root(doc), "fonts");
    size_t i, n;
    yyjson_val* font;
    u32 loaded = 0;
    yyjson_arr_foreach(fonts, i, n, font) {
        const char* file = yyjson_get_str(yyjson_obj_get(font, "file"));
        const char* family = yyjson_get_str(yyjson_obj_get(font, "family"));
        if (!file || !family) continue;
        const int weight = static_cast<int>(yyjson_get_int(yyjson_obj_get(font, "weight")));
        const char* style = yyjson_get_str(yyjson_obj_get(font, "style"));
        const bool fallback = yyjson_get_bool(yyjson_obj_get(font, "fallback"));
        const Rml::Style::FontStyle font_style = style && std::string_view(style) == "italic"
                                                     ? Rml::Style::FontStyle::Italic
                                                     : Rml::Style::FontStyle::Normal;
        const auto font_weight =
            weight > 0 ? static_cast<Rml::Style::FontWeight>(weight) : Rml::Style::FontWeight::Auto;
        if (Rml::LoadFontFace(std::string("fonts/") + file, family, font_style, font_weight, fallback)) ++loaded;
        else FORGE_ERROR("ui: cannot load font %s", file);
        // An icon font whose icons are also written as words (<i>home</i>), like its ligatures in a browser.
        if (yyjson_get_bool(yyjson_obj_get(font, "icon_names"))) Rml::SetFontFamilyWordGlyphs(family, icon_word_glyphs());
    }
    // Names of fonts that are not loaded, standing for loaded ones (font-family: Arial, sans-serif).
    yyjson_val* aliases = yyjson_obj_get(yyjson_doc_get_root(doc), "aliases");
    yyjson_val *key, *value;
    yyjson_obj_iter iter = yyjson_obj_iter_with(aliases);
    while ((key = yyjson_obj_iter_next(&iter))) {
        value = yyjson_obj_iter_get_val(key);
        if (yyjson_is_str(value)) Rml::SetFontFamilyAlias(yyjson_get_str(key), yyjson_get_str(value));
    }
    yyjson_doc_free(doc);
    FORGE_INFO("ui: %u font faces", loaded);
    return loaded > 0;
}

bool Ui::init(SDL_GPUDevice* device, SDL_Window* window, const UiConfig& config) {
    Impl& m = *impl_;
    m.device = device;
    m.window = window;
    m.config = config;

    m.tokens_path = config.root / "forge-ui" / "tokens.json";
    std::string error;
    if (!m.tokens.load(m.tokens_path, &error)) {
        FORGE_ERROR("ui: %s", error.c_str());
        return false;
    }
    if (!error.empty()) FORGE_WARN("ui: %s", error.c_str());
    if (!m.tokens.set_theme(config.theme)) FORGE_WARN("ui: unknown theme '%s'", config.theme.c_str());
    std::error_code ec;
    m.tokens_time = std::filesystem::last_write_time(m.tokens_path, ec);

    if (!m.renderer.init(device, config.msaa)) return false;
    m.files.set_root(config.root);
    m.files.set_tokens(&m.tokens);
    m.system.set_window(window);
    Rml::SetRenderInterface(&m.renderer);
    Rml::SetFileInterface(&m.files);
    Rml::SetSystemInterface(&m.system);
    if (!Rml::Initialise()) {
        FORGE_ERROR("ui: RmlUi failed to start");
        return false;
    }
    m.initialized = true;

    m.icon_instancer = std::make_unique<Rml::ElementInstancerGeneric<ElementIcon>>();
    m.list_instancer = std::make_unique<Rml::ElementInstancerGeneric<ElementVirtualList>>();
    Rml::Factory::RegisterElementInstancer("icon", m.icon_instancer.get());
    Rml::Factory::RegisterElementInstancer("virtual-list", m.list_instancer.get());
    m.lines_instancer = std::make_unique<Rml::ElementInstancerGeneric<ElementLines>>();
    m.shape_instancer = std::make_unique<Rml::ElementInstancerGeneric<ElementShape>>();
    Rml::Factory::RegisterElementInstancer("lines", m.lines_instancer.get());
    Rml::Factory::RegisterElementInstancer("shape", m.shape_instancer.get());

    if (!load_icon_codepoints(config.root / "fonts" / "icons.codepoints")) FORGE_WARN("ui: no icon names loaded");
    if (!m.load_fonts()) return false;
    m.next_poll_ns = time_now_ns();
    return true;
}

void Ui::shutdown() {
    Impl& m = *impl_;
    if (!m.initialized) return;
    m.documents.clear();
    m.contexts.clear();
    for (auto& [context, o] : m.offscreen) m.release_offscreen(o);
    m.offscreen.clear();
    Rml::Shutdown();
    m.renderer.shutdown();
    m.system.destroy_cursors();
    m.icon_instancer.reset();
    m.list_instancer.reset();
    m.lines_instancer.reset();
    m.shape_instancer.reset();
    m.initialized = false;
}

Rml::Context* Ui::create_context(const std::string& name, u32 width, u32 height) {
    Rml::Context* context =
        Rml::CreateContext(name, {static_cast<int>(width), static_cast<int>(height)});
    if (!context) return nullptr;
    context->SetDensityIndependentPixelRatio(impl_->config.dp_ratio);
    impl_->contexts.push_back(context);
    if (!impl_->debugger && impl_->config.debugger) impl_->debugger = Rml::Debugger::Initialise(context);
    return context;
}

Rml::ElementDocument* Ui::load_document(Rml::Context* context, const std::string& path) {
    Rml::ElementDocument* document = context->LoadDocument(path);
    if (!document) {
        FORGE_ERROR("ui: cannot load %s", path.c_str());
        return nullptr;
    }
    document->Show();
    impl_->documents.push_back({context, path, document});
    return document;
}

bool Ui::handle_event(Rml::Context* context, const SDL_Event& event) {
    // F8: RmlUi's element inspector, for working on documents and styles.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F8 && impl_->debugger) {
        Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
        return true;
    }
    return process_event(context, impl_->window, event);
}

void Ui::Impl::release_offscreen(Offscreen& o) {
    if (!o.image.empty()) {
        renderer.drop_external_texture(o.image);
        Rml::ReleaseTexture("/gpu/" + o.image);
        Rml::ReleaseTexture("gpu/" + o.image);
    }
    if (o.texture) SDL_ReleaseGPUTexture(device, o.texture);
    o = Offscreen{};
}

bool Ui::Impl::ensure_offscreen(Rml::Context* context, Offscreen& o) {
    const Rml::Vector2i size = context->GetDimensions();
    const u32 w = static_cast<u32>(std::max(size.x, 1)), h = static_cast<u32>(std::max(size.y, 1));
    if (o.texture && o.width == w && o.height == h) return true;
    const std::string image = o.image;
    release_offscreen(o);
    o.image = image;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    o.texture = SDL_CreateGPUTexture(device, &info);
    if (!o.texture) {
        FORGE_ERROR("ui: offscreen texture %ux%u: %s", w, h, SDL_GetError());
        return false;
    }
    o.width = w;
    o.height = h;
    renderer.set_external_texture(o.image, o.texture, w, h);
    return true;
}

void Ui::set_offscreen(Rml::Context* context, const std::string& image) {
    Impl& m = *impl_;
    auto it = m.offscreen.find(context);
    if (it != m.offscreen.end()) {
        if (it->second.image == image) return;
        m.release_offscreen(it->second);
        m.offscreen.erase(it);
    }
    if (image.empty()) return;
    Impl::Offscreen& o = m.offscreen[context];
    o.image = image;
    m.ensure_offscreen(context, o);
}

void Ui::set_active(Rml::Context* context, bool active) {
    if (active) impl_->inactive.erase(context);
    else impl_->inactive[context] = true;
}

void Ui::destroy_context(Rml::Context* context) {
    Impl& m = *impl_;
    set_offscreen(context, "");
    m.inactive.erase(context);
    m.documents.erase(std::remove_if(m.documents.begin(), m.documents.end(),
                                     [&](const Impl::Document& d) { return d.context == context; }),
                      m.documents.end());
    m.contexts.erase(std::remove(m.contexts.begin(), m.contexts.end(), context), m.contexts.end());
    Rml::RemoveContext(context->GetName());
}

void Ui::Impl::restyle() {
    for (Document& d : documents) d.document->ReloadStyleSheet();
}

void Ui::Impl::reload() {
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    for (Document& d : documents) {
        const bool visible = d.document->IsVisible();
        d.context->UnloadDocument(d.document);
        Rml::ElementDocument* fresh = d.context->LoadDocument(d.path);
        if (!fresh) {
            FORGE_ERROR("ui: reload of %s failed", d.path.c_str());
            continue;
        }
        if (visible) fresh->Show();
        d.document = fresh;
        ++stats.reloads;
    }
    if (on_reload) on_reload();
}

void Ui::Impl::poll_files() {
    if (!config.hot_reload) return;
    const u64 now = time_now_ns();
    if (now < next_poll_ns) return;
    next_poll_ns = now + 250'000'000; // four times a second
    FORGE_ZONE_N("UiHotReload");

    bool documents_changed = false, styles_changed = false;
    std::error_code ec;
    const auto tokens_now = std::filesystem::last_write_time(tokens_path, ec);
    if (!ec && tokens_now != tokens_time) {
        tokens_time = tokens_now;
        std::string error;
        const std::string theme = tokens.theme();
        if (tokens.load(tokens_path, &error)) {
            tokens.set_theme(theme);
            styles_changed = true;
            FORGE_INFO("ui: tokens reloaded");
        } else {
            FORGE_ERROR("ui: %s", error.c_str());
        }
    }
    for (const auto& [path, time] : files.watched()) {
        auto it = known.find(path);
        if (it == known.end()) {
            known[path] = time;
            continue;
        }
        const auto current = std::filesystem::last_write_time(utf8_path(path), ec);
        if (ec || current == it->second) continue;
        it->second = current;
        if (path.size() > 4 && path.compare(path.size() - 4, 4, ".rml") == 0) documents_changed = true;
        else styles_changed = true;
        FORGE_INFO("ui: %s changed", path.c_str());
    }
    if (documents_changed || styles_changed) {
        // Documents are re-read whole: styles may live inside them, and a
        // reload also picks up style changes.
        if (documents_changed) reload();
        else restyle();
        // What was just read is the new baseline.
        for (const auto& [path, time] : files.watched()) known[path] = time;
    }
}

void Ui::update() {
    FORGE_ZONE_N("UiUpdate");
    const u64 start = time_now_ns();
    impl_->poll_files();
    for (Rml::Context* context : impl_->contexts)
        if (!impl_->inactive.count(context)) context->Update();
    impl_->stats.update_ms = ns_to_ms(time_now_ns() - start);
}

void Ui::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, u32 width,
                u32 height) {
    FORGE_ZONE_N("UiRender");
    const u64 start = time_now_ns();
    Impl& m = *impl_;
    m.stats.draws = m.stats.passes = m.stats.uploads = 0;
    auto draw = [&](Rml::Context* context, SDL_GPUTexture* into, SDL_GPUTextureFormat into_format, u32 w, u32 h) {
        m.renderer.begin_frame(w, h);
        context->Render();
        m.renderer.end_frame(cmd, into, into_format);
        m.stats.draws += m.renderer.stats().draws;
        m.stats.passes += m.renderer.stats().passes;
        m.stats.uploads += m.renderer.stats().uploads;
    };
    // Offscreen contexts first: the others may show their pictures.
    for (Rml::Context* context : m.contexts) {
        auto it = m.offscreen.find(context);
        if (it == m.offscreen.end() || m.inactive.count(context)) continue;
        Impl::Offscreen& o = it->second;
        if (!m.ensure_offscreen(context, o)) continue;
        if (!cmd) continue;
        m.renderer.clear_texture(cmd, o.texture);
        draw(context, o.texture, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, o.width, o.height);
    }
    for (Rml::Context* context : m.contexts)
        if (!m.offscreen.count(context) && !m.inactive.count(context)) draw(context, target, format, width, height);
    m.stats.render_ms = ns_to_ms(time_now_ns() - start);
}

std::vector<std::string> Ui::themes() const { return impl_->tokens.themes(); }
const std::string& Ui::theme() const { return impl_->tokens.theme(); }

bool Ui::set_theme(const std::string& theme) {
    if (theme == impl_->tokens.theme()) return true;
    if (!impl_->tokens.set_theme(theme)) return false;
    impl_->restyle();
    return true;
}

void Ui::reload_documents() { impl_->reload(); }
const Tokens& Ui::tokens() const { return impl_->tokens; }
const std::filesystem::path& Ui::root() const { return impl_->config.root; }
const UiStats& Ui::stats() const { return impl_->stats; }

void Ui::set_image(const std::string& name, const u8* rgba, u32 width, u32 height) {
    // An uncompressed 32-bit TGA, which the image loader reads as is.
    std::string tga(18 + static_cast<usize>(width) * height * 4, '\0');
    tga[2] = 2; // true colour
    tga[12] = static_cast<char>(width & 0xff);
    tga[13] = static_cast<char>(width >> 8);
    tga[14] = static_cast<char>(height & 0xff);
    tga[15] = static_cast<char>(height >> 8);
    tga[16] = 32;
    tga[17] = 0x28; // 8 alpha bits, rows from the top
    char* p = tga.data() + 18;
    for (usize i = 0, n = static_cast<usize>(width) * height; i < n; ++i, p += 4) {
        p[0] = static_cast<char>(rgba[i * 4 + 2]); // BGRA
        p[1] = static_cast<char>(rgba[i * 4 + 1]);
        p[2] = static_cast<char>(rgba[i * 4 + 0]);
        p[3] = static_cast<char>(rgba[i * 4 + 3]);
    }
    impl_->files.set_memory_file(name, std::move(tga));
}
void Ui::drop_image(const std::string& name) {
    impl_->files.drop_memory_file(name);
    // The texture is cached under the path the document resolved.
    Rml::ReleaseTexture("memory/" + name);
    Rml::ReleaseTexture("/memory/" + name);
}
u32 Ui::msaa_samples() const { return impl_->renderer.msaa_samples(); }
void Ui::on_reload(std::function<void()> callback) { impl_->on_reload = std::move(callback); }

} // namespace forge::ui
