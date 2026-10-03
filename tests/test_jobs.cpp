#include "forge/core/jobs.h"

#include <doctest/doctest.h>

#include <atomic>
#include <vector>

using namespace forge;

namespace {
struct PoolScope {
    explicit PoolScope(u32 workers) { jobs::init(workers); }
    ~PoolScope() { jobs::shutdown(); }
};
} // namespace

TEST_CASE("parallel_for visits every index exactly once") {
    PoolScope pool(3);
    std::vector<std::atomic<u32>> hits(100'003);
    jobs::parallel_for(static_cast<u32>(hits.size()), 1000, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) hits[i].fetch_add(1, std::memory_order_relaxed);
    });
    u32 wrong = 0;
    for (auto& h : hits) wrong += h.load() != 1;
    CHECK(wrong == 0);
}

TEST_CASE("Submitting more jobs than a queue holds still runs them all") {
    PoolScope pool(2);
    constexpr u32 kCount = 20'000; // larger than one queue's capacity
    std::atomic<u32> ran{0};
    std::vector<Job> list(kCount, Job{[](void* data, u32) { static_cast<std::atomic<u32>*>(data)->fetch_add(1); }, &ran, 0});
    JobCounter counter;
    jobs::submit(list.data(), kCount, &counter);
    jobs::wait(counter);
    CHECK(ran.load() == kCount);
}

TEST_CASE("Jobs can spawn and wait for nested jobs") {
    PoolScope pool(3);
    std::atomic<u32> leaves{0};
    jobs::parallel_for(64, 1, [&](u32, u32) {
        jobs::parallel_for(64, 4, [&](u32 b, u32 e) { leaves.fetch_add(e - b); });
    });
    CHECK(leaves.load() == 64 * 64);
}

TEST_CASE("Repeated frames do not lose wake-ups") {
    PoolScope pool(3);
    std::atomic<u64> total{0};
    for (int frame = 0; frame < 2000; ++frame) {
        jobs::parallel_for(8, 1, [&](u32 b, u32 e) { total.fetch_add(e - b); });
    }
    CHECK(total.load() == 2000u * 8u);
}

TEST_CASE("Without a pool jobs run inline") {
    std::atomic<u32> ran{0};
    Job job{[](void* data, u32) { static_cast<std::atomic<u32>*>(data)->fetch_add(1); }, &ran, 0};
    JobCounter counter;
    jobs::submit(&job, 1, &counter);
    jobs::wait(counter);
    CHECK(ran.load() == 1);
}
