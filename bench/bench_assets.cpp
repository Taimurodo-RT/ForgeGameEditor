// Asset benchmark for the "50 000 assets" target:
//   - first scan of a fresh project (creates .meta files, hashes, cooks),
//   - rescan with nothing changed (what happens every time the editor
//     regains focus),
//   - search latency while typing in the asset browser.
//
//   forge_bench_assets [count]   (default 50 000)

#include "forge/assets/asset_pipeline.h"
#include "forge/core/jobs.h"
#include "forge/core/time.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace forge;
namespace fs = std::filesystem;

namespace {

const char* kFolders[] = {"Персонажи", "Враги", "Тайлы", "Предметы", "Звуки", "Музыка", "Интерфейс", "Эффекты", "Диалоги", "Карты"};
const char* kWords[] = {"кузнец", "меч", "щит", "трава", "камень", "вода", "огонь", "дверь", "сундук", "ключ",
                        "dragon", "slime", "forest", "castle", "potion", "arrow", "bow", "coin", "gem", "torch"};

} // namespace

int main(int argc, char** argv) {
    const u32 count = argc > 1 ? static_cast<u32>(std::strtoul(argv[1], nullptr, 10)) : 50'000;
    jobs::init();

    const fs::path root = fs::temp_directory_path() / ("forge_bench_assets_" + Guid::generate().to_string());
    const fs::path assets = root / "Assets";
    std::mt19937 rng(42);

    std::printf("creating %u files...\n", count);
    for (u32 i = 0; i < count; ++i) {
        const char* folder = kFolders[i % 10];
        const std::string name = std::string(kWords[rng() % 20]) + "_" + kWords[rng() % 20] + "_" + std::to_string(i) + ".txt";
        const fs::path p = assets / folder / std::to_string(i % 50) / name;
        if (i < 500) fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << "asset " << i << " payload " << std::string(64 + i % 200, 'x');
    }

    {
        assets::AssetPipeline pipeline(assets, root / "Library");
        std::string error;
        if (!pipeline.open(&error)) {
            std::printf("open failed: %s\n", error.c_str());
            return 1;
        }
        pipeline.add_default_importers();

        const assets::RefreshReport cold = pipeline.refresh();
        std::printf("first scan      %9.1f ms   files %u, cooked %u, failed %u\n", cold.ms, cold.files, cold.cooked, cold.failed);
        const assets::RefreshReport warm = pipeline.refresh();
        std::printf("rescan, no edit %9.1f ms   unchanged %u, cooked %u\n", warm.ms, warm.unchanged, warm.cooked);

        // Typing in the search box: each keystroke is one query.
        std::vector<std::string> queries;
        for (const char* w : kWords) {
            const std::string word = w;
            for (usize n = 1; n <= word.size(); ++n) queries.push_back(word.substr(0, n));
        }
        queries.push_back("меч кузнец");
        queries.push_back("dragon castle 12");
        std::vector<f64> times;
        usize hits = 0;
        for (int round = 0; round < 3; ++round) {
            for (const std::string& q : queries) {
                const u64 t0 = time_now_ns();
                hits += pipeline.database().search(q, 200).size();
                times.push_back(ns_to_ms(time_now_ns() - t0));
            }
        }
        std::sort(times.begin(), times.end());
        std::printf("search          median %.2f ms, p99 %.2f ms, worst %.2f ms over %zu queries (target < 50 ms)\n",
                    times[times.size() / 2], times[times.size() * 99 / 100], times.back(), times.size());
        (void)hits;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    jobs::shutdown();
    return 0;
}
