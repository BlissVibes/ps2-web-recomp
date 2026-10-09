#include "queue.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#else
#include <chrono>
#include <thread>
#endif

namespace ps2::gs::queue {

namespace {

std::unique_ptr<std::uint8_t[]> g_buffer;
std::uint64_t g_capacity = 0;
bool g_active = false;
alignas(64) std::atomic<std::uint64_t> g_head{0};
alignas(64) std::atomic<std::uint64_t> g_tail{0};
alignas(64) std::atomic<std::uint32_t> g_posted{0};
alignas(64) std::atomic<std::uint32_t> g_freed{0};
std::atomic<std::uint32_t> g_consumer_waiting{0}, g_producer_waiting{0};

constexpr std::uint64_t align16(std::uint64_t n) { return (n + 15) & ~15ull; }

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

void notify(std::atomic<std::uint32_t> &value) {
#ifdef __EMSCRIPTEN__
    emscripten_futex_wake(&value, INT_MAX);
#else
    value.notify_all();
#endif
}

bool active() { return g_active; }

void start(std::size_t capacity) {
    g_capacity = capacity;
    g_buffer = std::make_unique<std::uint8_t[]>(capacity);
    g_active = true;
}

void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {
    const std::uint64_t need = sizeof(Header) + align16(bytes);
    std::uint64_t head = g_head.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t pos = head % g_capacity;
        const std::uint64_t pad = pos + need > g_capacity ? g_capacity - pos : 0;
        const std::uint32_t seen = g_freed.load(std::memory_order_acquire);
        if (head + pad + need - g_tail.load(std::memory_order_acquire) <= g_capacity) {
            if (pad) {
                const Header wrap{kWrap, 0, 0, 0};
                std::memcpy(g_buffer.get() + pos, &wrap, sizeof wrap);
                head += pad;
            }
            break;
        }
        g_producer_waiting.store(1);
        if (head + pad + need - g_tail.load() > g_capacity) wait(g_freed, seen);
    }
    std::uint8_t *at = g_buffer.get() + head % g_capacity;
    const Header h{type, bytes, arg0, arg1};
    std::memcpy(at, &h, sizeof h);
    if (bytes) std::memcpy(at + sizeof h, data, bytes);
    g_head.store(head + need, std::memory_order_release);
    g_posted.fetch_add(1);
    if (g_consumer_waiting.exchange(0)) notify(g_posted);
}

bool peek(Header &header, const std::uint8_t *&payload) {
    std::uint64_t tail = g_tail.load(std::memory_order_relaxed);
    const std::uint64_t head = g_head.load(std::memory_order_acquire);
    while (tail != head) {
        const std::uint64_t pos = tail % g_capacity;
        std::memcpy(&header, g_buffer.get() + pos, sizeof header);
        if (header.type != kWrap) {
            payload = g_buffer.get() + pos + sizeof header;
            return true;
        }
        tail += g_capacity - pos;
        g_tail.store(tail, std::memory_order_release);
    }
    return false;
}

void pop(const Header &header) {
    g_tail.store(g_tail.load(std::memory_order_relaxed) + sizeof(Header) + align16(header.bytes), std::memory_order_release);
    g_freed.fetch_add(1);
    if (g_producer_waiting.exchange(0)) notify(g_freed);
}

void wait_for_work(int timeout_ms) {
    const std::uint32_t seen = g_posted.load();
    g_consumer_waiting.store(1);
    if (g_head.load() != g_tail.load()) {
        g_consumer_waiting.store(0);
        return;
    }
#ifdef __EMSCRIPTEN__
    ps2_wait_async(&g_posted, seen, timeout_ms);
#else
    (void)seen;
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 1)));
#endif
}

}
