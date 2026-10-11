#pragma once

// What the game and the level editor share of a «Три в ряд» level (step 14.3b, 14.3-три-в-ряд-модель.md, «Уровни»):
// the blocks' components, the game's set of gems, the field as the author drew it (cells of the layer «Поле», gems
// put on them before, one copy of «Правила уровня»), how a board is drawn, and the level module the editor paints
// with.

#include "match3_rules.h"

#include "forge/data/reflect.h"
#include "forge/level/level.h"
#include "forge/objects/library.h"
#include "forge/render/camera.h"
#include "forge/render/sprite_batch.h"
#include "forge/render/sprite_renderer.h"

#include <SDL3/SDL_gpu.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace match3 {

// «Фишка»: Gem.points, Gem.sound, Gem.on_board.
struct Gem {
    u32 points = 10;
    std::string sound;
    bool on_board = true;
};
// «Правила уровня»: what a level plays by (the copy's values, the template's where the copy has none of its own).
struct BoardRules {
    u32 moves = 20;
    u32 target = 1000;
    u32 seed = 0;
    f32 four = 1.5f, five = 2.0f, chain = 0.5f;
};
void register_components(forge::scene::Scene& scene);

// The kinds' and blocks' ids (games/modules/match3/kinds.json).
inline constexpr std::string_view kGemBlock = "gem";
inline constexpr std::string_view kRulesBlock = "board_rules";

// The level's one layer and its tiles: a cell of the field, light or dark (the rules see both alike: a cell).
inline constexpr forge::u32 kFieldLayer = 0;
inline constexpr forge::world::TileId kCell = 1, kCellDark = 2;
inline bool is_cell(forge::world::TileId t) { return t == kCell || t == kCellDark; }
// The field is read within this many tiles of the level's origin each way; cells further out are not looked at.
inline constexpr i32 kReach = 32;

// The game's set of gems: every template with the block «Фишка» and «На поле» on, by template id. A rename, the order
// of the files or of the list in «Объекты» does not change it.
struct GemSet {
    std::vector<const forge::objects::Template*> gems;
    Scoring scoring; // «Очки» by gem number
    std::vector<std::string> sounds;
    // A template's gem number; -1: not in the set.
    i32 index(std::string_view template_id) const;
    u32 size() const { return static_cast<u32>(gems.size()); }
};
GemSet gem_set(const forge::objects::Library& library);

// A level's field as the author made it.
struct Field {
    Board shape;        // its cells (kNone), holes, and the gems put before (their numbers in the set)
    i32 x0 = 0, y0 = 0; // the level's tile of the board's top left cell
    std::vector<bool> dark; // by cell: drawn with «Клетка тёмная»
    Rules rules;
    u32 rules_copies = 0; // of «Правила уровня» (0: the kind's values)
};
// Reads it from an open level (its objects loaded). False, why in the author's words («Поле «Первый»: …»): no cell,
// more than kMaxSide cells each way, a gem on no cell or of a template not in the set, two copies of «Правила
// уровня». A field read may still not start (Play::start: too few gems, presets in a row, no move).
bool read_field(forge::level::Level& level, const forge::objects::Library& library, const GemSet& set,
                const std::string& name, Field& out, std::string& why);

// The middle of a cell in the level's tiles.
inline f64 cell_x(const Field& f, i32 x) { return f.x0 + x + 0.5; }
inline f64 cell_y(const Field& f, i32 y) { return f.y0 + y + 0.5; }

// Draws boards: the cells, the gems by their templates' pictures (frame 0 on the board, frames 1… while they vanish;
// a gem with no picture is a coloured disc of its own), a frame round the field and a cell picked. The game and the
// editor draw with it alike.
class Painter {
public:
    bool init(SDL_GPUDevice* device, SDL_GPUTextureFormat format);
    void shutdown();
    bool ready() const { return ready_; }
    // The sheet made anew when the set's pictures changed (templates, files); outside any render pass.
    void update(const forge::objects::Library& library, const GemSet& set);

    u32 white() const { return 0; }
    u32 cell(bool dark) const { return dark ? 2 : 1; }
    // A gem's frame of the sheet: frame 0 stands, 1… vanish; how many it has.
    u32 gem_frame(u32 gem, u32 frame) const;
    u32 gem_frames(u32 gem) const;
    // The colour a gem without a picture is drawn in (and the pixels a test looks for).
    static forge::u32 placeholder(u32 gem);

    forge::render::SpriteBatch& batch() { return batch_; }
    // Each frame: begin(), push sprites into batch(), prepare() outside a render pass, draw() inside one.
    void begin(const forge::render::Camera2D& camera);
    void prepare(SDL_GPUCommandBuffer* cmd, const forge::render::Camera2D& camera, u32 width, u32 height);
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass);

    // Pushes a sprite of a frame at a point of the level (tiles), size in tiles.
    void put(u32 frame, f64 x, f64 y, f32 w, f32 h, u32 color = 0xffffffffu, u32 order = 1);
    // A field's cells as the author drew them, and the frame round it (over the gems: they pass under it).
    void put_cells(const Field& field);
    // What hides the gems where the field has no cells: above it (new gems come from there) and its holes (falling
    // gems pass them). In the colour of what is behind the field.
    void put_covers(const Field& field, u32 color);

private:
    void make_sheet(const forge::objects::Library& library, const GemSet& set);

    bool ready_ = false;
    forge::render::SpriteRenderer sprites_;
    forge::render::SpriteBatch batch_;
    std::vector<u8> rgba_;
    u32 width_ = 0, height_ = 0;
    std::vector<forge::render::SpriteRect> frames_;
    std::vector<u32> first_, count_; // by gem: its first frame, how many
    std::string signature_ = "-";
    bool sheet_changed_ = false;
};

// The editor's view of a «Три в ряд» level: the layer «Поле» with the tiles «Клетка» and «Клетка тёмная», nothing
// around, the field's cells and gems put before, its frame; objects are the game's templates (gems, the rules).
class Match3Level final : public forge::level::LevelModule {
public:
    Match3Level();
    ~Match3Level() override;

    // The game's objects: its kinds.json and objects/.
    bool load_objects(const std::filesystem::path& game_dir, std::string* error = nullptr);
    const forge::objects::Library& library_ref() const { return library_; }

    std::string title() const override { return "Три в ряд"; }
    forge::world::WorldDesc world_desc() const override;
    std::shared_ptr<const forge::world::Generator> generator() const override { return gen_; }
    void setup_scene(forge::scene::Scene& scene) override;
    const std::vector<std::string>& layer_names() const override { return layers_; }
    const std::vector<forge::level::TileDef>& tiles() const override { return tiles_; }
    void tile_icon(const forge::level::TileDef& tile, u32 size, std::vector<u8>& rgba) const override;
    // The middle of the field: of its cells as drawn, else of the place a new field is drawn at.
    void start(f64& x, f64& y) const override;
    u32 map_color(u32 layer, forge::world::TileId value) const override;
    forge::Color background() const override;

    bool init_view(SDL_GPUDevice* device, SDL_GPUTextureFormat format) override;
    void shutdown_view() override;
    void prepare_view(SDL_GPUCommandBuffer* cmd, forge::level::Level& level, const forge::render::Camera2D& camera,
                      u32 width, u32 height, const forge::level::ViewOptions& options, f64 time) override;
    void draw_view(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) override;

    forge::objects::Library* library() override { return &library_; }
    u64 objects_version() const override { return library_.version(); }
    const std::vector<forge::level::ObjectDef>& objects() const override;
    void object_icon(const forge::level::ObjectDef& def, u32 size, std::vector<u8>& rgba) const override;
    // A gem or the rules: in the middle of the cell under the point.
    flecs::entity place_object(forge::level::Level& level, usize index, f64 x, f64 y) override;
    i32 object_kind(flecs::entity e) const override;
    bool object_box(flecs::entity e, f64& x0, f64& y0, f64& x1, f64& y1) const override;

    // What the field of a level says now (for the status line and the tests): "" when it starts.
    std::string field_problem(forge::level::Level& level, const std::string& name) const;

private:
    forge::objects::Library library_;
    std::shared_ptr<const forge::world::Generator> gen_;
    std::vector<std::string> layers_ = {"Поле"};
    std::vector<forge::level::TileDef> tiles_;
    mutable std::vector<forge::level::ObjectDef> defs_;
    mutable u64 defs_version_ = ~0ull;
    mutable f64 start_x_ = 4, start_y_ = 4;
    Painter painter_;
};

} // namespace match3

FORGE_REFLECT_DECLARE(match3::Gem)
FORGE_REFLECT_DECLARE(match3::BoardRules)
