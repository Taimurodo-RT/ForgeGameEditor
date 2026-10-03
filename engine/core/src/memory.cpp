#include "forge/core/memory.h"
#include "forge/core/profile.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace forge {

namespace {
std::atomic<usize> g_bytes[static_cast<usize>(MemoryCategory::Count)];
}

const char* memory_category_name(MemoryCategory category) {
    switch (category) {
    case MemoryCategory::General: return "general";
    case MemoryCategory::Frame: return "frame";
    case MemoryCategory::World: return "world";
    case MemoryCategory::Render: return "render";
    case MemoryCategory::Audio: return "audio";
    case MemoryCategory::Assets: return "assets";
    case MemoryCategory::Scripts: return "scripts";
    case MemoryCategory::UI: return "ui";
    case MemoryCategory::Count: break;
    }
    return "?";
}

void* mem_alloc(usize size, usize align, MemoryCategory category) {
    if (align < alignof(std::max_align_t)) align = alignof(std::max_align_t);
#if defined(_MSC_VER)
    void* ptr = _aligned_malloc(size, align);
#else
    void* ptr = std::aligned_alloc(align, align_up(size, align));
#endif
    if (ptr) {
        g_bytes[static_cast<usize>(category)].fetch_add(size, std::memory_order_relaxed);
        TracyAlloc(ptr, size);
    }
    return ptr;
}

void mem_free(void* ptr, usize size, MemoryCategory category) {
    if (!ptr) return;
    TracyFree(ptr);
    g_bytes[static_cast<usize>(category)].fetch_sub(size, std::memory_order_relaxed);
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

MemoryStats memory_stats() {
    MemoryStats stats{};
    for (usize i = 0; i < static_cast<usize>(MemoryCategory::Count); ++i) {
        stats.bytes[i] = g_bytes[i].load(std::memory_order_relaxed);
    }
    return stats;
}

// --- LinearArena -----------------------------------------------------------

LinearArena::LinearArena(usize capacity, MemoryCategory category)
    : capacity_(capacity), category_(category) {
    base_ = static_cast<u8*>(mem_alloc(capacity, kCacheLine, category));
    FORGE_VERIFY(base_ != nullptr);
}

LinearArena::~LinearArena() { release(); }

LinearArena::LinearArena(LinearArena&& other) noexcept
    : base_(other.base_), capacity_(other.capacity_), used_(other.used_), peak_(other.peak_),
      category_(other.category_) {
    other.base_ = nullptr;
    other.capacity_ = other.used_ = other.peak_ = 0;
}

LinearArena& LinearArena::operator=(LinearArena&& other) noexcept {
    if (this != &other) {
        release();
        base_ = other.base_;
        capacity_ = other.capacity_;
        used_ = other.used_;
        peak_ = other.peak_;
        category_ = other.category_;
        other.base_ = nullptr;
        other.capacity_ = other.used_ = other.peak_ = 0;
    }
    return *this;
}

void LinearArena::release() {
    mem_free(base_, capacity_, category_);
    base_ = nullptr;
}

void* LinearArena::alloc(usize size, usize align) {
    const usize start = align_up(reinterpret_cast<usize>(base_) + used_, align) - reinterpret_cast<usize>(base_);
    if (start + size > capacity_) return nullptr;
    used_ = start + size;
    if (used_ > peak_) peak_ = used_;
    return base_ + start;
}

// --- BlockPool -------------------------------------------------------------

BlockPool::BlockPool(usize block_size, usize block_align, usize blocks_per_page, MemoryCategory category)
    : block_align_(block_align < alignof(FreeNode) ? alignof(FreeNode) : block_align),
      blocks_per_page_(blocks_per_page), category_(category) {
    block_size_ = align_up(block_size < sizeof(FreeNode) ? sizeof(FreeNode) : block_size, block_align_);
    page_header_ = align_up(sizeof(Page), block_align_);
    FORGE_VERIFY(blocks_per_page_ > 0);
}

BlockPool::~BlockPool() {
    FORGE_ASSERT(live_ == 0);
    const usize page_bytes = page_header_ + block_size_ * blocks_per_page_;
    while (pages_) {
        Page* next = pages_->next;
        mem_free(pages_, page_bytes, category_);
        pages_ = next;
    }
}

void BlockPool::grow() {
    const usize page_bytes = page_header_ + block_size_ * blocks_per_page_;
    auto* page = static_cast<Page*>(mem_alloc(page_bytes, block_align_, category_));
    FORGE_VERIFY(page != nullptr);
    page->next = pages_;
    pages_ = page;
    ++page_count_;

    u8* blocks = reinterpret_cast<u8*>(page) + page_header_;
    // Thread the new blocks onto the free list back to front so they are
    // handed out in address order, which keeps early allocations contiguous.
    for (usize i = blocks_per_page_; i-- > 0;) {
        auto* node = reinterpret_cast<FreeNode*>(blocks + i * block_size_);
        node->next = free_list_;
        free_list_ = node;
    }
}

void* BlockPool::alloc() {
    if (!free_list_) grow();
    FreeNode* node = free_list_;
    free_list_ = node->next;
    ++live_;
    return node;
}

void BlockPool::free(void* block) {
    if (!block) return;
    FORGE_ASSERT(live_ > 0);
    auto* node = static_cast<FreeNode*>(block);
    node->next = free_list_;
    free_list_ = node;
    --live_;
}

} // namespace forge
