#include "forge/core/handle.h"

#include <doctest/doctest.h>

using namespace forge;

TEST_CASE("HandlePool detects stale handles") {
    HandlePool<int> pool;
    auto a = pool.insert(1);
    auto b = pool.insert(2);
    CHECK(*pool.get(a) == 1);
    CHECK(pool.remove(a));
    CHECK(pool.get(a) == nullptr);
    CHECK_FALSE(pool.remove(a));

    auto c = pool.insert(3); // reuses a's slot
    CHECK(c.index == a.index);
    CHECK(pool.get(a) == nullptr);
    CHECK(*pool.get(c) == 3);
    CHECK(*pool.get(b) == 2);
}

TEST_CASE("HandlePool keeps values packed after removal") {
    HandlePool<int> pool;
    Handle<int> h[5];
    for (int i = 0; i < 5; ++i) h[i] = pool.insert(i * 10);
    pool.remove(h[1]);
    pool.remove(h[3]);
    CHECK(pool.size() == 3);
    int sum = 0;
    for (int v : pool) sum += v;
    CHECK(sum == 0 + 20 + 40);
    CHECK(*pool.get(h[4]) == 40);
}

TEST_CASE("Null handle is never valid") {
    HandlePool<int> pool;
    pool.insert(7);
    CHECK(pool.get(Handle<int>{}) == nullptr);
}
