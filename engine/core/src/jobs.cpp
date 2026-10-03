#include "forge/core/jobs.h"

#include "forge/core/assert.h"
#include "forge/core/log.h"
#include "forge/core/profile.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#define FORGE_CPU_PAUSE() _mm_pause()
#elif defined(__x86_64__) || defined(__i386__)
#define FORGE_CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define FORGE_CPU_PAUSE() asm volatile("yield")
#else
#define FORGE_CPU_PAUSE() ((void)0)
#endif

namespace forge::jobs {

namespace {

struct QueuedJob {
    Job job;
    JobCounter* counter;
};

// Small test-and-test-and-set lock. Queue operations hold it for a few
// instructions, so spinning is cheaper than a kernel mutex.
class SpinLock {
public:
    void lock() {
        for (;;) {
            if (!flag_.exchange(true, std::memory_order_acquire)) return;
            while (flag_.load(std::memory_order_relaxed)) FORGE_CPU_PAUSE();
        }
    }
    bool try_lock() { return !flag_.load(std::memory_order_relaxed) && !flag_.exchange(true, std::memory_order_acquire); }
    void unlock() { flag_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> flag_{false};
};

// Fixed-capacity ring. The owner pushes and pops at the back (LIFO, warm
// caches); thieves take from the front (oldest, usually the biggest work).
class alignas(kCacheLine) WorkQueue {
public:
    static constexpr u32 kCapacity = 4096;

    bool push(const QueuedJob& job) {
        std::lock_guard lock(lock_);
        if (back_ - front_ == kCapacity) return false;
        items_[back_ & (kCapacity - 1)] = job;
        ++back_;
        return true;
    }

    // Pushes as many as fit under one lock; returns how many were taken.
    u32 push_batch(const Job* list, u32 count, JobCounter* counter) {
        std::lock_guard lock(lock_);
        const u32 free_slots = static_cast<u32>(kCapacity - (back_ - front_));
        const u32 n = count < free_slots ? count : free_slots;
        for (u32 i = 0; i < n; ++i) items_[(back_ + i) & (kCapacity - 1)] = {list[i], counter};
        back_ += n;
        return n;
    }

    bool pop(QueuedJob& out) {
        std::lock_guard lock(lock_);
        if (back_ == front_) return false;
        --back_;
        out = items_[back_ & (kCapacity - 1)];
        return true;
    }

    // Takes up to half of the queue (at most max) from the front in one lock.
    u32 steal_batch(QueuedJob* out, u32 max) {
        if (!lock_.try_lock()) return 0;
        const u64 size = back_ - front_;
        u32 n = static_cast<u32>((size + 1) / 2);
        if (n > max) n = max;
        for (u32 i = 0; i < n; ++i) out[i] = items_[(front_ + i) & (kCapacity - 1)];
        front_ += n;
        lock_.unlock();
        return n;
    }

    // Puts stolen jobs into the owner's queue; they are already counted in pending.
    u32 push_stolen(const QueuedJob* list, u32 count) {
        std::lock_guard lock(lock_);
        const u32 free_slots = static_cast<u32>(kCapacity - (back_ - front_));
        const u32 n = count < free_slots ? count : free_slots;
        for (u32 i = 0; i < n; ++i) items_[(back_ + i) & (kCapacity - 1)] = list[i];
        back_ += n;
        return n;
    }

    bool steal(QueuedJob& out) {
        if (!lock_.try_lock()) return false;
        bool ok = false;
        if (back_ != front_) {
            out = items_[front_ & (kCapacity - 1)];
            ++front_;
            ok = true;
        }
        lock_.unlock();
        return ok;
    }

private:
    SpinLock lock_;
    u64 front_ = 0;
    u64 back_ = 0;
    QueuedJob items_[kCapacity];
};

struct Pool {
    std::vector<std::unique_ptr<WorkQueue>> queues;
    std::vector<std::thread> threads;
    std::atomic<bool> quit{false};

    // Jobs queued but not yet taken; idle workers sleep while both are zero.
    alignas(kCacheLine) std::atomic<u32> pending{0};
    alignas(kCacheLine) std::atomic<u32> background_pending{0};
    alignas(kCacheLine) std::atomic<u32> sleepers{0};
    std::mutex sleep_mutex;
    std::condition_variable wake;

    // Background work is rare next to frame jobs (hundreds per second, not
    // tens of thousands per frame), so one shared FIFO is enough.
    SpinLock background_lock;
    std::deque<QueuedJob> background;
};

Pool* g_pool = nullptr;
thread_local u32 t_index = ~0u;
thread_local u32 t_steal_seed = 0;

void execute(const QueuedJob& q) {
    q.job.fn(q.job.data, q.job.arg);
    if (q.counter) q.counter->value.fetch_sub(1, std::memory_order_acq_rel);
}

bool try_take(Pool& pool, u32 self, QueuedJob& out) {
    const u32 n = static_cast<u32>(pool.queues.size());
    if (self < n && pool.queues[self]->pop(out)) {
        pool.pending.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }
    // Start stealing at a different victim each time to spread contention.
    const u32 start = t_steal_seed++;
    for (u32 i = 0; i < n; ++i) {
        const u32 victim = (start + i) % n;
        if (victim == self) continue;
        if (self >= n) {
            if (pool.queues[victim]->steal(out)) {
                pool.pending.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
            continue;
        }
        // Steal a batch: run the first, keep the rest locally so the next jobs
        // come from our own queue instead of contending on the victim's lock.
        constexpr u32 kMaxSteal = 64;
        QueuedJob stolen[kMaxSteal];
        const u32 got = pool.queues[victim]->steal_batch(stolen, kMaxSteal);
        if (got == 0) continue;
        pool.pending.fetch_sub(1, std::memory_order_relaxed);
        out = stolen[0];
        if (got > 1) {
            const u32 kept = pool.queues[self]->push_stolen(stolen + 1, got - 1);
            // Our queue had no room for some (rare): run them now.
            for (u32 k = 1 + kept; k < got; ++k) {
                pool.pending.fetch_sub(1, std::memory_order_relaxed);
                execute(stolen[k]);
            }
        }
        return true;
    }
    return false;
}

bool try_take_background(Pool& pool, QueuedJob& out) {
    if (pool.background_pending.load(std::memory_order_relaxed) == 0) return false;
    std::lock_guard lock(pool.background_lock);
    if (pool.background.empty()) return false;
    out = pool.background.front();
    pool.background.pop_front();
    pool.background_pending.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

bool has_work(const Pool& pool) {
    return pool.pending.load(std::memory_order_acquire) > 0 || pool.background_pending.load(std::memory_order_acquire) > 0;
}

void worker_main(Pool& pool, u32 index) {
    t_index = index;
    t_steal_seed = index * 7919u;
    char name[32];
    std::snprintf(name, sizeof(name), "Worker %u", index);
    FORGE_THREAD_NAME(name);

    QueuedJob job;
    while (!pool.quit.load(std::memory_order_acquire)) {
        if (try_take(pool, index, job)) {
            execute(job);
            continue;
        }
        if (try_take_background(pool, job)) {
            FORGE_ZONE_N("Background job");
            execute(job);
            continue;
        }
        // Spin briefly: in a frame, more jobs usually arrive within microseconds.
        bool found = false;
        for (int spin = 0; spin < 256 && !found; ++spin) {
            FORGE_CPU_PAUSE();
            if (has_work(pool)) found = true;
        }
        if (found) continue;

        std::unique_lock lock(pool.sleep_mutex);
        pool.sleepers.fetch_add(1, std::memory_order_relaxed);
        pool.wake.wait(lock, [&] { return has_work(pool) || pool.quit.load(std::memory_order_acquire); });
        pool.sleepers.fetch_sub(1, std::memory_order_relaxed);
    }
}

void wake_workers(Pool& pool, u32 queued) {
    if (pool.sleepers.load(std::memory_order_relaxed) == 0) return;
    // Taking the lock orders this wake-up after a worker's predicate check.
    { std::lock_guard lock(pool.sleep_mutex); }
    if (queued == 1) pool.wake.notify_one();
    else pool.wake.notify_all();
}

} // namespace

void init(u32 worker_count) {
    FORGE_VERIFY(g_pool == nullptr);
    if (worker_count == 0) {
        // One thread per core, the calling thread included, minus one core
        // left to the OS and other programs on bigger CPUs: when every core is
        // ours, the OS preempts a worker in the middle of a batch and the
        // whole parallel_for waits a scheduler quantum (~20 ms on Windows).
        const u32 hw = std::thread::hardware_concurrency();
        worker_count = hw >= 8 ? hw - 2 : (hw > 1 ? hw - 1 : 1);
    }
    g_pool = new Pool();
    const u32 total = worker_count + 1;
    for (u32 i = 0; i < total; ++i) g_pool->queues.push_back(std::make_unique<WorkQueue>());

    t_index = 0;
    t_steal_seed = 0;
    for (u32 i = 1; i < total; ++i) g_pool->threads.emplace_back(worker_main, std::ref(*g_pool), i);
    FORGE_INFO("job system: %u threads", total);
}

void shutdown() {
    if (!g_pool) return;
    // Finish queued background work so nothing that owns memory is dropped.
    QueuedJob job;
    while (try_take_background(*g_pool, job)) execute(job);
    {
        std::lock_guard lock(g_pool->sleep_mutex);
        g_pool->quit.store(true, std::memory_order_release);
    }
    g_pool->wake.notify_all();
    for (auto& t : g_pool->threads) t.join();
    delete g_pool;
    g_pool = nullptr;
    t_index = ~0u;
}

u32 thread_count() { return g_pool ? static_cast<u32>(g_pool->queues.size()) : 1; }
u32 this_thread_index() { return t_index; }

void submit(const Job* list, u32 count, JobCounter* counter) {
    if (count == 0) return;
    if (counter) counter->value.fetch_add(count, std::memory_order_relaxed);

    // Without a pool (or from a foreign thread) jobs simply run inline.
    if (!g_pool || t_index == ~0u) {
        for (u32 i = 0; i < count; ++i) execute({list[i], counter});
        return;
    }

    Pool& pool = *g_pool;
    WorkQueue& queue = *pool.queues[t_index];
    u32 done = 0;
    while (done < count) {
        // Count jobs before they become visible so a thief never finds a job
        // while pending still reads zero; give back what did not fit.
        const u32 want = count - done;
        pool.pending.fetch_add(want, std::memory_order_release);
        const u32 pushed = queue.push_batch(list + done, want, counter);
        if (pushed < want) pool.pending.fetch_sub(want - pushed, std::memory_order_relaxed);
        done += pushed;
        if (pushed > 0) wake_workers(pool, pushed);
        if (done < count) {
            // Queue full: make room by running one of our own jobs.
            QueuedJob job;
            if (try_take(pool, t_index, job)) execute(job);
        }
    }
}

void submit_background(const Job& job, JobCounter* counter) {
    if (counter) counter->value.fetch_add(1, std::memory_order_relaxed);
    // Without workers nobody would ever pick it up: run it now.
    if (!g_pool || g_pool->threads.empty()) {
        execute({job, counter});
        return;
    }
    Pool& pool = *g_pool;
    {
        std::lock_guard lock(pool.background_lock);
        pool.background.push_back({job, counter});
        pool.background_pending.fetch_add(1, std::memory_order_release);
    }
    wake_workers(pool, 1);
}

u32 background_pending() { return g_pool ? g_pool->background_pending.load(std::memory_order_relaxed) : 0; }

void wait(JobCounter& counter) {
    FORGE_ZONE();
    if (!g_pool || t_index == ~0u) {
        while (!counter.done()) std::this_thread::yield();
        return;
    }
    QueuedJob job;
    while (!counter.done()) {
        if (try_take(*g_pool, t_index, job)) execute(job);
        else FORGE_CPU_PAUSE();
    }
}

} // namespace forge::jobs
