#pragma once

#include "forge/core/assert.h"
#include "forge/core/types.h"

#include <vector>

namespace forge {

// A reference to an object in a HandlePool: a slot index plus the generation
// the slot had when the object was created. Once the object is destroyed the
// generation moves on, so stale handles are detected instead of dangling.
template <typename Tag>
struct Handle {
    u32 index = 0;
    u32 generation = 0; // 0 is never a live generation, so Handle{} is null.

    bool is_null() const { return generation == 0; }
    explicit operator bool() const { return !is_null(); }
    friend bool operator==(Handle a, Handle b) { return a.index == b.index && a.generation == b.generation; }
};

// Dense storage addressed by handles. Values live in one contiguous array that
// stays packed on removal (swap with last), so iterating all live values is a
// straight walk over memory.
template <typename T, typename Tag = T>
class HandlePool {
public:
    using HandleType = Handle<Tag>;

    HandleType insert(T value) {
        u32 slot;
        if (free_head_ != kNone) {
            slot = free_head_;
            free_head_ = slots_[slot].dense_or_next;
        } else {
            slot = static_cast<u32>(slots_.size());
            slots_.push_back({kNone, 0});
        }
        Slot& s = slots_[slot];
        s.generation = next_generation(s.generation);
        s.dense_or_next = static_cast<u32>(values_.size());
        values_.push_back(static_cast<T&&>(value));
        dense_to_slot_.push_back(slot);
        return {slot, s.generation};
    }

    bool contains(HandleType h) const {
        return h.generation != 0 && h.index < slots_.size() && slots_[h.index].generation == h.generation;
    }

    T* get(HandleType h) { return contains(h) ? &values_[slots_[h.index].dense_or_next] : nullptr; }
    const T* get(HandleType h) const { return contains(h) ? &values_[slots_[h.index].dense_or_next] : nullptr; }

    bool remove(HandleType h) {
        if (!contains(h)) return false;
        Slot& s = slots_[h.index];
        const u32 dense = s.dense_or_next;
        const u32 last = static_cast<u32>(values_.size() - 1);
        if (dense != last) {
            values_[dense] = static_cast<T&&>(values_[last]);
            dense_to_slot_[dense] = dense_to_slot_[last];
            slots_[dense_to_slot_[dense]].dense_or_next = dense;
        }
        values_.pop_back();
        dense_to_slot_.pop_back();
        // Bump the generation now so the handle is stale even before reuse.
        s.generation = next_generation(s.generation);
        s.dense_or_next = free_head_;
        free_head_ = h.index;
        return true;
    }

    usize size() const { return values_.size(); }
    T* begin() { return values_.data(); }
    T* end() { return values_.data() + values_.size(); }
    const T* begin() const { return values_.data(); }
    const T* end() const { return values_.data() + values_.size(); }

private:
    static constexpr u32 kNone = ~u32{0};

    struct Slot {
        u32 dense_or_next; // dense index while alive, next free slot while free
        u32 generation;
    };

    static u32 next_generation(u32 g) { return g + 1 == 0 ? 1 : g + 1; }

    std::vector<Slot> slots_;
    std::vector<T> values_;
    std::vector<u32> dense_to_slot_;
    u32 free_head_ = kNone;
};

} // namespace forge
