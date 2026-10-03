// Sandbox: the first runnable piece of Forge. Opens a window, keeps a frame
// loop going and pushes a realistic amount of parallel work through the job
// system every frame, so the frame time can be watched in the title bar and
// in Tracy.
//
//   forge_sandbox                 window, runs until closed
//   forge_sandbox --headless 600  no window, 600 frames, prints a summary

#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/memory.h"
#include "forge/core/profile.h"
#include "forge/platform/app.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace forge;

namespace {

// Stand-in for entity data until the ECS lands: plain arrays, one per field.
struct Particles {
    std::vector<f32> x, y, vx, vy;

    explicit Particles(u32 count) : x(count), y(count), vx(count), vy(count) {
        for (u32 i = 0; i < count; ++i) {
            const f32 a = static_cast<f32>(i) * 0.618f;
            x[i] = std::cos(a) * 1000.0f;
            y[i] = std::sin(a) * 1000.0f;
            vx[i] = -y[i] * 0.01f;
            vy[i] = x[i] * 0.01f;
        }
    }
    u32 size() const { return static_cast<u32>(x.size()); }
};

class Sandbox final : public App {
public:
    static constexpr u32 kParticles = 200'000; // the "active objects" target
    static constexpr u32 kSmallJobs = 10'000;  // step 1 check: 10 000 jobs per frame

    bool on_init() override {
        frame_arena_ = LinearArena(16 * MiB, MemoryCategory::Frame);
        FORGE_INFO("sandbox: %u particles, %u jobs per frame on %u threads", kParticles, kSmallJobs,
                   jobs::thread_count());
        return true;
    }

    void on_frame(f64 dt) override {
        frame_arena_.reset();
        const f32 step = static_cast<f32>(dt > 0.05 ? 0.05 : dt);

        {
            FORGE_ZONE_N("Simulate particles");
            jobs::parallel_for(particles_.size(), 4096, [&](u32 begin, u32 end) {
                FORGE_ZONE_N("particle batch");
                for (u32 i = begin; i < end; ++i) {
                    // Pull toward the centre: a cheap orbit that never flies off.
                    particles_.vx[i] -= particles_.x[i] * 0.0001f;
                    particles_.vy[i] -= particles_.y[i] * 0.0001f;
                    particles_.x[i] += particles_.vx[i] * step;
                    particles_.y[i] += particles_.vy[i] * step;
                }
            });
        }

        {
            FORGE_ZONE_N("Many small jobs");
            // Each job writes its own slot of a frame-arena array; tests both the
            // queue under load and the per-frame allocator.
            u64* results = frame_arena_.alloc_array<u64>(kSmallJobs);
            Job* list = frame_arena_.alloc_array<Job>(kSmallJobs);
            for (u32 i = 0; i < kSmallJobs; ++i) {
                list[i] = {[](void* data, u32 arg) {
                               u64 h = arg * 0x9E3779B97F4A7C15ull;
                               for (int k = 0; k < 16; ++k) h ^= h >> 29, h *= 0xBF58476D1CE4E5B9ull;
                               static_cast<u64*>(data)[arg] = h;
                           },
                           results, i};
            }
            JobCounter counter;
            jobs::submit(list, kSmallJobs, &counter);
            jobs::wait(counter);
        }
    }

    void on_shutdown() override {
        const FrameStats& s = frame_stats();
        FORGE_INFO("frames %llu, avg %.3f ms, worst %.3f ms (last %u frames)",
                   static_cast<unsigned long long>(s.frame), s.avg_ms, s.worst_ms, FrameStats::kWindow);
        FORGE_INFO("frame arena peak %.1f KiB", static_cast<double>(frame_arena_.peak()) / 1024.0);
    }

private:
    Particles particles_{kParticles};
    LinearArena frame_arena_;
};

} // namespace

int main(int argc, char** argv) {
    AppConfig config;
    config.title = "Forge Sandbox";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            config.headless = true;
            if (i + 1 < argc) config.max_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--no-vsync") == 0) {
            config.vsync = false;
        }
    }
    Sandbox app;
    return app.run(config);
}
