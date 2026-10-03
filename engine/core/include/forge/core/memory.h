#pragma once

#include "forge/core/assert.h"
#include "forge/core/types.h"

#include <new>
#include <type_traits>
#include <utility>

namespace forge {

// Every allocation is charged to a category so the editor and profiler can
// show where memory goes and warn when a budget is exceeded.
enum class MemoryCategory : u8 {
    General,
    Frame,
    World,
    Render,
    Audio,
    Assets,
    Scripts,
    UI,
    Count
};

struct MemoryStats {
    usize bytes[static_cast<usize>(MemoryCategory::Count)];
};

const char* memory_category_name(MemoryCategory category);

// Aligned heap allocation charged to a category.
void* mem_alloc(usize size, usize align, MemoryCategory category);
void mem_free(void* ptr, usize size, MemoryCategory category);

MemoryStats memory_stats();

inline usize align_up(usize value, usize align) {
    FORGE_ASSERT((align & (align - 1)) == 0);
    return (value + align - 1) & ~(align - 1);
}

// Bump allocator over one fixed block. Allocation is a pointer increment and
// everything is released at once with reset(). Used for per-frame scratch data.
class LinearArena {
public:
    LinearArena() = default;
    LinearArena(usize capacity, MemoryCategory category);
    ~LinearArena();

    LinearArena(const LinearArena&) = delete;
    LinearArena& operator=(const LinearArena&) = delete;
    LinearArena(LinearArena&& other) noexcept;
    LinearArena& operator=(LinearArena&& other) noexcept;

    // Returns nullptr when the arena is full; callers decide how to degrade.
    void* alloc(usize size, usize align = alignof(std::max_align_t));

    template <typename T>
    T* alloc_array(usize count) {
        static_assert(std::is_trivially_destructible_v<T>, "arena memory is never destructed");
        void* memory = alloc(sizeof(T) * count, alignof(T));
        return memory ? static_cast<T*>(memory) : nullptr;
    }

    template <typename T, typename... Args>
    T* make(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "arena memory is never destructed");
        void* memory = alloc(sizeof(T), alignof(T));
        return memory ? new (memory) T(std::forward<Args>(args)...) : nullptr;
    }

    usize marker() const { return used_; }
    void rewind(usize marker) { FORGE_ASSERT(marker <= used_); used_ = marker; }
    void reset() { used_ = 0; }

    usize used() const { return used_; }
    usize capacity() const { return capacity_; }
    usize peak() const { return peak_; }

private:
    void release();

    u8* base_ = nullptr;
    usize capacity_ = 0;
    usize used_ = 0;
    usize peak_ = 0;
    MemoryCategory category_ = MemoryCategory::General;
};

// Fixed-size block allocator with an intrusive free list. O(1) alloc/free and
// no fragmentation; grows by whole pages of blocks when it runs out.
class BlockPool {
public:
    BlockPool(usize block_size, usize block_align, usize blocks_per_page, MemoryCategory category);
    ~BlockPool();

    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    void* alloc();
    void free(void* block);

    usize live_blocks() const { return live_; }
    usize page_count() const { return page_count_; }

private:
    struct FreeNode { FreeNode* next; };
    struct Page { Page* next; };

    void grow();

    usize block_size_;
    usize block_align_;
    usize blocks_per_page_;
    usize page_header_;
    MemoryCategory category_;
    FreeNode* free_list_ = nullptr;
    Page* pages_ = nullptr;
    usize page_count_ = 0;
    usize live_ = 0;
};

} // namespace forge
