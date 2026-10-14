#include "queue.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#else
#include <chrono>
#include <thread>
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif
#endif

namespace ps2::gs::queue {

namespace {

constexpr std::uint64_t align16(std::uint64_t n) { return (n + 15) & ~15ull; }

Ring g_render, g_vu;
thread_local Role g_role = Role::Emulation;

}

#ifdef __EMSCRIPTEN__
// The main thread cannot block: it suspends until the word changes or the time runs out. Returns the
// milliseconds it was suspended, timed here so that the caller needs no clock of its own.
EM_ASYNC_JS(double, ps2_wait_async, (std::atomic<std::uint32_t> *word, std::uint32_t seen, int timeout_ms), {
    const start = performance.now();
    const result = Atomics.waitAsync(HEAP32, word >> 2, seen, timeout_ms);
    if (!result.async) return 0;
    await result.value;
    return performance.now() - start;
});
#endif

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
// Natively only the tests use the rings. They wait as the page's threads do, on a futex that only a wake
// ends, so that a wake the protocol fails to send shows as a wait that runs out its time.
namespace {
int futex(std::atomic<std::uint32_t> &word, int op, std::uint32_t value, const timespec *timeout) {
    return static_cast<int>(syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), op, value, timeout, nullptr, 0));
}
}
#endif

void wait(std::atomic<std::uint32_t> &value, std::uint32_t seen) {
#ifdef __EMSCRIPTEN__
    emscripten_futex_wait(&value, seen, INFINITY);
#elif defined(__linux__)
    futex(value, FUTEX_WAIT_PRIVATE, seen, nullptr);
#else
    value.wait(seen);
#endif
}

// As wait(), for at most `ms` milliseconds. False when the time ran out.
bool wait_for(std::atomic<std::uint32_t> &value, std::uint32_t seen, int ms) {
#ifdef __EMSCRIPTEN__
    return emscripten_futex_wait(&value, seen, ms) != -ETIMEDOUT;
#elif defined(__linux__)
    const timespec timeout{ms / 1000, (ms % 1000) * 1000000L};
    return !(futex(value, FUTEX_WAIT_PRIVATE, seen, &timeout) == -1 && errno == ETIMEDOUT);
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
#elif defined(__linux__)
    futex(value, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr);
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

void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {
    if (type == kGif && g_role == Role::Vu) {
        g_render.add_gif(arg0, data, bytes);
        return;
    }
    outbox().post(type, arg0, arg1, data, bytes);
}

void stats() {
    const auto line = [](const char *name, const Ring &r) {
        if (!r.active()) return;
        std::fprintf(stderr, "[queue] %s ring of %llu bytes: at most %llu waiting, largest message %llu, poster waited for room %llu times, lost wakes %llu; "
                     "%llu messages in %llu sends, %llu of which woke the reader\n", name,
                     static_cast<unsigned long long>(r.capacity()), static_cast<unsigned long long>(r.high_water()),
                     static_cast<unsigned long long>(r.largest()), static_cast<unsigned long long>(r.waits()), static_cast<unsigned long long>(r.lost_wakes()),
                     static_cast<unsigned long long>(r.messages()), static_cast<unsigned long long>(r.sends()), static_cast<unsigned long long>(r.wakes()));
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
    std::fprintf(stderr, "[stuck] %s ring: head %llu, tail %llu, %llu bytes waiting; sent %llu times, freed %u; reader waiting %u, poster waiting %u\n", name,
                 static_cast<unsigned long long>(head), static_cast<unsigned long long>(tail), static_cast<unsigned long long>(head - tail),
                 static_cast<unsigned long long>(sends()), freed_.load(), consumer_waiting_.load(), producer_waiting_.load());
}

void describe() {
    g_vu.describe("vu");
    g_render.describe("render");
}

void Ring::start(std::size_t capacity) {
    buffer_ = std::make_unique<std::uint8_t[]>(capacity);
    capacity_ = capacity;
    send_each_ = std::getenv("PS2_RING_EACH") != nullptr;
}

void Ring::write_header(std::uint64_t at, const Header &h) const { std::memcpy(buffer_.get() + at % capacity_, &h, sizeof h); }

std::uint64_t Ring::reserve(std::uint64_t need) {
    open_ = ~0ull;   // a new message: nothing more joins the one before
    bool waited = false;
    for (;;) {
        // A message that does not fit before the end of the buffer goes to its start. The rest of the buffer
        // is given up first and on its own, as soon as it is free: the reader then passes it, and an empty
        // ring has room for any message up to its capacity wherever the head stood.
        const std::uint64_t pos = write_ % capacity_;
        const std::uint64_t pad = pos + need > capacity_ ? capacity_ - pos : 0;
        const std::uint64_t claim = pad ? pad : need;   // what is taken at the head this time round
        if (write_ + claim - tail_seen_ > capacity_) tail_seen_ = tail_.load(std::memory_order_acquire);
        if (write_ + claim - tail_seen_ <= capacity_) {
            if (!pad) {
                const std::uint64_t used = write_ + need - tail_seen_;
                if (used > high_water_.load(std::memory_order_relaxed)) high_water_.store(used, std::memory_order_relaxed);
                return write_;
            }
            write_header(write_, Header{kWrap, 0, 0, 0});
            write_ += pad;
            continue;
        }
        // No room. What is written and not sent must reach the reader first: it is what the reader frees.
        send();
        const std::uint32_t seen = freed_.load(std::memory_order_acquire);
        tail_seen_ = tail_.load(std::memory_order_acquire);
        if (write_ + claim - tail_seen_ <= capacity_) continue;
        if (!waited) waits_.store(waits_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        waited = true;
        producer_waiting_.store(1);
        // The reader wakes a poster that waits, but in the page a wake has been seen to go missing (the poster
        // slept on with the ring empty, 2026-10-13): the wait also ends after 2 ms and looks again.
        if (write_ + claim - tail_.load() > capacity_ && !wait_for(freed_, seen, 2) && freed_.load() != seen) lost_wakes_.fetch_add(1);
    }
}

void Ring::post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {
    const std::uint64_t need = sizeof(Header) + align16(bytes);
    if (need > capacity_) {
        std::fprintf(stderr, "[queue] a message of %u bytes (type %u) does not fit the ring of %llu bytes: dropped\n", bytes,
                     static_cast<unsigned>(type), static_cast<unsigned long long>(capacity_));
        return;
    }
    const std::uint64_t at = reserve(need);
    if (need > largest_.load(std::memory_order_relaxed)) largest_.store(need, std::memory_order_relaxed);
    write_header(at, Header{type, bytes, arg0, arg1});
    if (bytes) std::memcpy(buffer_.get() + at % capacity_ + sizeof(Header), data, bytes);
    write_ = at + need;
    messages_.store(messages_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    send();
}

void Ring::add_gif(std::uint32_t path, const void *data, std::uint32_t bytes) {
    if (!bytes) return;
    if (send_each_) {
        post(kGif, path, bytes / 16, data, bytes);
        return;
    }
    const auto *from = static_cast<const std::uint8_t *>(data);
    while (bytes) {
        // Joins the open message when it is of the same path, has room under kMaxPayload, and the data fits
        // before the end of the buffer and in what the reader has freed.
        const std::uint32_t joins = open_ == ~0ull || open_path_ != path ? 0 : std::min(bytes, kMaxPayload - open_bytes_);
        const bool fits = joins && open_ % capacity_ + sizeof(Header) + open_bytes_ + joins <= capacity_ &&
                          (write_ + joins - tail_seen_ <= capacity_ || write_ + joins - (tail_seen_ = tail_.load(std::memory_order_acquire)) <= capacity_);
        if (fits) {
            std::memcpy(buffer_.get() + write_ % capacity_, from, joins);
            write_ += joins;
            open_bytes_ += joins;
            write_header(open_, Header{kGif, open_bytes_, path, open_bytes_ / 16});
            if (sizeof(Header) + open_bytes_ > largest_.load(std::memory_order_relaxed)) largest_.store(sizeof(Header) + open_bytes_, std::memory_order_relaxed);
            const std::uint64_t used = write_ - tail_seen_;
            if (used > high_water_.load(std::memory_order_relaxed)) high_water_.store(used, std::memory_order_relaxed);
            from += joins;
            bytes -= joins;
            unsent_ += joins;
        } else {
            const std::uint32_t n = std::min(bytes, kMaxPayload);
            const std::uint64_t need = sizeof(Header) + align16(n);
            if (need > capacity_) {
                std::fprintf(stderr, "[queue] a message of %u bytes (type %u) does not fit the ring of %llu bytes: dropped\n", n,
                             static_cast<unsigned>(kGif), static_cast<unsigned long long>(capacity_));
                return;
            }
            const std::uint64_t at = reserve(need);
            if (need > largest_.load(std::memory_order_relaxed)) largest_.store(need, std::memory_order_relaxed);
            write_header(at, Header{kGif, n, path, n / 16});
            std::memcpy(buffer_.get() + at % capacity_ + sizeof(Header), from, n);
            write_ = at + need;
            messages_.store(messages_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            open_ = at;
            open_path_ = path;
            open_bytes_ = n;
            from += n;
            bytes -= n;
            unsent_ += n;
        }
        if (unsent_ >= kSendBytes) send();
    }
}

void Ring::send() {
    open_ = ~0ull;   // the reader may read the header from now on: it must not change
    unsent_ = 0;
    if (head_.load(std::memory_order_relaxed) == write_) return;
    head_.store(write_);
    tail_seen_ = tail_.load(std::memory_order_acquire);   // once a send, so that room and high_water() stay near the truth
    sends_.store(sends_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (consumer_waiting_.load() && consumer_waiting_.exchange(0)) {
        notify(consumer_waiting_);
        wakes_.store(wakes_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
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
    if (consumer_waiting_.exchange(0)) notify(consumer_waiting_);
}

double Ring::wait_for_work(int timeout_ms) {
    consumer_waiting_.store(1);
    if (head_.load() != tail_.load()) {
        consumer_waiting_.store(0);
        return 0.0;
    }
    double waited;
#ifdef __EMSCRIPTEN__
    if (emscripten_is_main_browser_thread()) {
        waited = ps2_wait_async(&consumer_waiting_, 1, timeout_ms);
    } else {
        const double start = emscripten_get_now();
        emscripten_futex_wait(&consumer_waiting_, 1, timeout_ms);
        waited = emscripten_get_now() - start;
    }
#else
    const auto start = std::chrono::steady_clock::now();
    wait_for(consumer_waiting_, 1, timeout_ms);   // natively only the tests wait here
    waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
#endif
    consumer_waiting_.store(0);
    return waited;
}

}
