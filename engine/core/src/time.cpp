#include "forge/core/time.h"

#include <chrono>

namespace forge {

u64 time_now_ns() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace forge
