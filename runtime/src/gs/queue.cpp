#include "queue.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#else
#include <chrono>
#include <thread>
#endif

namespace ps2::gs::queue {

namespace {

constexpr std::uint64_t align16(std::uint64_t n) { return (n + 15) & ~15ull; }

Ring g_render, g_vu;
thread_local Role g_role = Role::Emulation;

}

#ifdef __EMSCRIPTEN__
EM_ASYNC_JS(void, ps2_wait_async, (std::atomic<std::uint32_t> *word, std::uint32_t seen, int timeout_ms), {
    const result = Atomics.waitAsync(HEAP32, word >> 2, seen, timeout_ms);
    if (result.async) await result.value;
});
#endif

void wait(std::atomic<std::uint32_t> &value, std::uint32_t seen) {
#ifdef __EMSCRIPTEN__
    emscripten_futex_wait(&value, seen, INFINITY);
#else
    value.wait(seen);
#endif
}

// As wait(), for at most `ms` milliseconds. False when the time ran out.
bool wait_for(std::atomic<std::uint32_t> &value, std::uint32_t seen, int ms) {
#ifdef __EMSCRIPTEN__
    return emscripten_futex_wait(&value, seen, ms) != -ETIMEDOUT;
#else
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (value.load() == seen)
        if (std::chrono::steady_clock::now() >= until) return false;
        else std::this_thread::yield();
    return true;
#endif
}

void notify(std::atomic<std::uint32_t> &value) {
#ifdef __EMSCRIPTEN__
    emscripten_futex_wake(&value, INT_MAX);
#else
    value.notify_all();
#endif
}

Ring &render() { return g_render; }
Ring &vu() { return g_vu; }
Role &role() { return g_role; }

Ring &outbox() {
    if (g_role == Role::Emulation && g_vu.active()) return g_vu;
    return g_render;
}

bool active() { return g_render.active(); }
bool vu_active() { return g_vu.active(); }

void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) { outbox().post(type, arg0, arg1, data, bytes); }

void stats() {
    const auto line = [](const char *name, const Ring &r) {
        if (!r.active()) return;
        std::fprintf(stderr, "[queue] %s ring of %llu bytes: at most %llu waiting, largest message %llu, poster waited for room %llu times, lost wakes %llu\n", name,
                     static_cast<unsigned long long>(r.capacity()), static_cast<unsigned long long>(r.high_water()),
                     static_cast<unsigned long long>(r.largest()), static_cast<unsigned long long>(r.waits()), static_cast<unsigned long long>(r.lost_wakes()));
    };
    line("vu", g_vu);
    line("render", g_render);
}

#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE void ps2_ring_stats() { stats(); }
#endif

void Ring::describe(const char *name) const {
    if (!active()) return;
    const std::uint64_t head = head_.load(), tail = tail_.load();
    std::fprintf(stderr, "[stuck] %s ring: head %llu, tail %llu, %llu bytes waiting; posted %u, freed %u; reader waiting %u, poster waiting %u\n", name,
                 static_cast<unsigned long long>(head), static_cast<unsigned long long>(tail), static_cast<unsigned long long>(head - tail),
                 posted_.load(), freed_.load(), consumer_waiting_.load(), producer_waiting_.load());
}

void describe() {
    g_vu.describe("vu");
    g_render.describe("render");
}

void Ring::start(std::size_t capacity) {
    buffer_ = std::make_unique<std::uint8_t[]>(capacity);
    capacity_ = capacity;
}

void Ring::post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {
    const std::uint64_t need = sizeof(Header) + align16(bytes);
    if (need > capacity_) {
        std::fprintf(stderr, "[queue] a message of %u bytes (type %u) does not fit the ring of %llu bytes: dropped\n", bytes,
                     static_cast<unsigned>(type), static_cast<unsigned long long>(capacity_));
        return;
    }
    std::uint64_t head = head_.load(std::memory_order_relaxed);
    bool waited = false;
    for (;;) {
        // A message that does not fit before the end of the buffer goes to its start. The rest of the buffer
        // is given up first and on its own, as soon as it is free: the reader then passes it, and an empty
        // ring has room for any message up to its capacity wherever the head stood.
        const std::uint64_t pos = head % capacity_;
        const std::uint64_t pad = pos + need > capacity_ ? capacity_ - pos : 0;
        const std::uint64_t claim = pad ? pad : need;   // what is taken at the head this time round
        const std::uint32_t seen = freed_.load(std::memory_order_acquire);
        const std::uint64_t used = head - tail_.load(std::memory_order_acquire);
        if (used + claim <= capacity_) {
            if (!pad) {
                if (used + need > high_water_.load(std::memory_order_relaxed)) high_water_.store(used + need, std::memory_order_relaxed);
                break;
            }
            const Header wrap{kWrap, 0, 0, 0};
            std::memcpy(buffer_.get() + pos, &wrap, sizeof wrap);
            head += pad;
            head_.store(head, std::memory_order_release);
            wake();
            continue;
        }
        if (!waited) waits_.store(waits_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        waited = true;
        producer_waiting_.store(1);
        // The reader wakes a poster that waits, but in the page a wake has been seen to go missing (the poster
        // slept on with the ring empty, 2026-10-13): the wait also ends after 2 ms and looks again.
        if (head + claim - tail_.load() > capacity_ && !wait_for(freed_, seen, 2) && freed_.load() != seen) lost_wakes_.fetch_add(1);
    }
    if (need > largest_.load(std::memory_order_relaxed)) largest_.store(need, std::memory_order_relaxed);
    std::uint8_t *at = buffer_.get() + head % capacity_;
    const Header h{type, bytes, arg0, arg1};
    std::memcpy(at, &h, sizeof h);
    if (bytes) std::memcpy(at + sizeof h, data, bytes);
    head_.store(head + need, std::memory_order_release);
    posted_.fetch_add(1);
    if (consumer_waiting_.exchange(0)) notify(posted_);
}

bool Ring::peek(Header &header, const std::uint8_t *&payload) {
    std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    while (tail != head) {
        const std::uint64_t pos = tail % capacity_;
        std::memcpy(&header, buffer_.get() + pos, sizeof header);
        if (header.type != kWrap) {
            payload = buffer_.get() + pos + sizeof header;
            return true;
        }
        tail += capacity_ - pos;
        tail_.store(tail, std::memory_order_release);
        freed_.fetch_add(1);                                 // the end of the buffer is room the poster may be waiting for
        if (producer_waiting_.exchange(0)) notify(freed_);
    }
    return false;
}

void Ring::pop(const Header &header) {
    tail_.store(tail_.load(std::memory_order_relaxed) + sizeof(Header) + align16(header.bytes), std::memory_order_release);
    freed_.fetch_add(1);
    if (producer_waiting_.exchange(0)) notify(freed_);
}

void Ring::wake() {
    posted_.fetch_add(1);
    if (consumer_waiting_.exchange(0)) notify(posted_);
}

void Ring::wait_for_work(int timeout_ms) {
    const std::uint32_t seen = posted_.load();
    consumer_waiting_.store(1);
    if (head_.load() != tail_.load()) {
        consumer_waiting_.store(0);
        return;
    }
#ifdef __EMSCRIPTEN__
    if (emscripten_is_main_browser_thread()) ps2_wait_async(&posted_, seen, timeout_ms);
    else emscripten_futex_wait(&posted_, seen, timeout_ms);
#else
    (void)seen;
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 1)));
#endif
}

}
