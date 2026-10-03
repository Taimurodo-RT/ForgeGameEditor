#pragma once

#include "forge/core/types.h"

#include <atomic>
#include <type_traits>

namespace forge {

// Counts unfinished jobs of one batch. wait() returns when it reaches zero.
struct JobCounter {
    std::atomic<u32> value{0};
    bool done() const { return value.load(std::memory_order_acquire) == 0; }
};

using JobFn = void (*)(void* data, u32 arg);

struct Job {
    JobFn fn = nullptr;
    void* data = nullptr;
    u32 arg = 0;
};

// One pool of worker threads shared by the whole engine: ECS systems, physics,
// streaming, asset cooking and rendering all submit here instead of owning
// threads. Each thread has its own queue; idle threads steal from others.
// The thread that calls init() becomes worker 0 and runs jobs while waiting.
namespace jobs {

// worker_count = 0 picks one thread per hardware thread, the calling one included,
// leaving one free for the OS on CPUs with 8 or more.
void init(u32 worker_count = 0);
void shutdown();

u32 thread_count();      // workers + the main thread
u32 this_thread_index(); // 0 for the main thread, ~0u for threads outside the pool

// Queue jobs. If counter is given it is increased by count before any job runs
// and decreased as each finishes.
void submit(const Job* jobs, u32 count, JobCounter* counter);

// Queue long-running work that no frame waits on: chunk generation, loading,
// saving, cooking. Background jobs run on worker threads only when no regular
// job is queued, so they fill idle time and never hold up the frame. wait()
// never picks them up, so a frame waiting on its own jobs is not stalled
// behind them. Keep each job short (well under a millisecond): a worker in the
// middle of one cannot help the frame until it finishes. Without a pool the
// job runs inline.
void submit_background(const Job& job, JobCounter* counter);
u32 background_pending(); // queued and not yet started

// Run other jobs until the counter reaches zero. Safe to call from inside a job.
void wait(JobCounter& counter);

// Split [0, count) into batches and run fn(begin, end) for each, in parallel.
// Blocks until all batches finish. fn must be safe to call concurrently.
template <typename Fn>
void parallel_for(u32 count, u32 batch_size, Fn&& fn);

} // namespace jobs

// --- implementation details ------------------------------------------------

namespace jobs::detail {
template <typename Fn>
struct ForContext {
    Fn* fn;
    u32 count;
    u32 batch_size;
};

template <typename Fn>
void run_batch(void* data, u32 batch) {
    auto* ctx = static_cast<ForContext<Fn>*>(data);
    const u32 begin = batch * ctx->batch_size;
    const u32 end = begin + ctx->batch_size < ctx->count ? begin + ctx->batch_size : ctx->count;
    (*ctx->fn)(begin, end);
}
} // namespace jobs::detail

template <typename Fn>
void jobs::parallel_for(u32 count, u32 batch_size, Fn&& fn) {
    if (count == 0) return;
    if (batch_size == 0) batch_size = 1;
    using F = std::remove_reference_t<Fn>;
    detail::ForContext<F> ctx{&fn, count, batch_size};
    const u32 batches = (count + batch_size - 1) / batch_size;
    if (batches == 1) {
        fn(0u, count);
        return;
    }

    JobCounter counter;
    constexpr u32 kChunk = 256;
    Job chunk[kChunk];
    for (u32 first = 0; first < batches; first += kChunk) {
        const u32 n = batches - first < kChunk ? batches - first : kChunk;
        for (u32 i = 0; i < n; ++i) chunk[i] = {&detail::run_batch<F>, &ctx, first + i};
        submit(chunk, n, &counter);
    }
    wait(counter);
}

} // namespace forge
