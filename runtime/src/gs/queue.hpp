#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ps2::gs::queue {

enum Type : std::uint32_t { kWrap, kGif, kGifReset, kReset, kVertexReset, kVblank, kPresent, kDownload, kVif1, kVif1Write, kVuReset, kState, kVuFence };

enum class Role { Emulation, Vu, Render };

struct Header {
    std::uint32_t type, bytes, arg0, arg1;
};

// The most payload a poster hands over as one message: one DMA transfer (65,535 qwords) fits, and GS data
// longer than this goes as several messages (gif.cpp). A ring takes any message up to its own capacity, but
// such a message waits until the ring is empty, so the rings are many times this size.
inline constexpr std::uint32_t kMaxPayload = 1u << 20;

// The two rings of the page. Both are filled with zeros at the start and written round and round, so their
// whole size stays in use. In the race (native run, frames 12300 to 14100) the game's texture uploads are
// 3.3 MB a frame, measured, and pass through both rings; the drawing data is estimated at 3 MB more in the
// render ring (62,000 vertices a frame at three qwords each), so a frame is about 7 MB there. The reader of
// the render ring waits for the display once a frame while the next frame arrives, so about one frame waits
// at the most in normal running: 16 MB is twice that. What the VU ring carries beside the uploads (the VIF1
// data) has not been measured. It gets 16 MB as well, roughly the three frames the emulation may run ahead
// of the display (gs::post_present), so that a slow VU frame stops the emulation no sooner than it did.
// stats() tells how full they have been.
inline constexpr std::size_t kRenderRingBytes = 16u << 20, kVuRingBytes = 16u << 20;

class Ring {
public:
    void start(std::size_t capacity);   // a multiple of 16
    bool active() const { return capacity_ != 0; }
    // Waits for room. A message larger than the whole ring is reported and dropped: no wait would end.
    void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
    bool peek(Header &header, const std::uint8_t *&payload);
    void pop(const Header &header);
    void wait_for_work(int timeout_ms);
    void wake();
    std::uint64_t capacity() const { return capacity_; }
    std::uint64_t high_water() const { return high_water_.load(std::memory_order_relaxed); }   // most bytes waiting at once
    std::uint64_t largest() const { return largest_.load(std::memory_order_relaxed); }         // largest message, with its header
    std::uint64_t waits() const { return waits_.load(std::memory_order_relaxed); }             // times the poster found no room

private:
    std::unique_ptr<std::uint8_t[]> buffer_;
    std::uint64_t capacity_ = 0;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> high_water_{0}, largest_{0}, waits_{0};   // written by the poster only
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    alignas(64) std::atomic<std::uint32_t> posted_{0};
    alignas(64) std::atomic<std::uint32_t> freed_{0};
    std::atomic<std::uint32_t> consumer_waiting_{0}, producer_waiting_{0};
};

Ring &render();
Ring &vu();
Role &role();
Ring &outbox();

bool active();
bool vu_active();
void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
void stats();   // how full each ring has been, to stderr (in the page: Module._ps2_ring_stats() from the console)

void wait(std::atomic<std::uint32_t> &value, std::uint32_t seen);
void notify(std::atomic<std::uint32_t> &value);

}
