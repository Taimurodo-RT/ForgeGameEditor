#include "forge/core/sort.h"

#include "forge/core/jobs.h"
#include "forge/core/profile.h"

#include <algorithm>
#include <vector>

namespace forge {

u64* radix_sort_by_high32(u64* data, u64* scratch, u32 n) {
    FORGE_ZONE_N("Radix sort");
    if (n < 2) return data;
    constexpr u32 kBlock = 1u << 14;
    constexpr u32 kDigits = 256;
    const u32 blocks = (n + kBlock - 1) / kBlock;

    // Which key bits differ anywhere: a byte that never changes needs no pass.
    std::vector<u32> any(blocks), all(blocks);
    jobs::parallel_for(blocks, 1, [&](u32 b, u32 e) {
        for (u32 k = b; k < e; ++k) {
            u32 o = 0, a = ~0u;
            const u32 end = std::min(n, (k + 1) * kBlock);
            for (u32 i = k * kBlock; i < end; ++i) {
                const u32 key = static_cast<u32>(data[i] >> 32);
                o |= key;
                a &= key;
            }
            any[k] = o;
            all[k] = a;
        }
    });
    u32 varying = 0, common = ~0u;
    for (u32 k = 0; k < blocks; ++k) {
        varying |= any[k];
        common &= all[k];
    }
    varying ^= common;

    std::vector<u32> offsets(static_cast<usize>(blocks) * kDigits);
    u64* src = data;
    u64* dst = scratch;
    for (u32 pass = 0; pass < 4; ++pass) {
        const u32 shift = 32 + pass * 8;
        if (((varying >> (pass * 8)) & 0xffu) == 0) continue;
        std::fill(offsets.begin(), offsets.end(), 0u);
        jobs::parallel_for(blocks, 1, [&](u32 b, u32 e) {
            for (u32 k = b; k < e; ++k) {
                u32* count = offsets.data() + static_cast<usize>(k) * kDigits;
                const u32 end = std::min(n, (k + 1) * kBlock);
                for (u32 i = k * kBlock; i < end; ++i) ++count[(src[i] >> shift) & 0xffu];
            }
        });
        // Digit by digit, block by block: equal keys keep their order.
        u32 running = 0;
        for (u32 d = 0; d < kDigits; ++d)
            for (u32 k = 0; k < blocks; ++k) {
                u32& slot = offsets[static_cast<usize>(k) * kDigits + d];
                const u32 c = slot;
                slot = running;
                running += c;
            }
        jobs::parallel_for(blocks, 1, [&](u32 b, u32 e) {
            for (u32 k = b; k < e; ++k) {
                u32* next = offsets.data() + static_cast<usize>(k) * kDigits;
                const u32 end = std::min(n, (k + 1) * kBlock);
                for (u32 i = k * kBlock; i < end; ++i) dst[next[(src[i] >> shift) & 0xffu]++] = src[i];
            }
        });
        std::swap(src, dst);
    }
    return src;
}

} // namespace forge
