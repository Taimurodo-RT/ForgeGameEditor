#pragma once

// Parallel radix sort for draw lists and other big per-frame arrays.

#include "forge/core/types.h"

namespace forge {

// Sorts n values by their upper 32 bits, keeping the original order of equal
// keys (put a payload, such as an index, in the lower 32 bits). Uses every job
// thread; bytes of the key that are the same in all values are skipped.
// scratch must hold n values. Returns data or scratch, whichever ends up
// holding the sorted result.
u64* radix_sort_by_high32(u64* data, u64* scratch, u32 n);

} // namespace forge
