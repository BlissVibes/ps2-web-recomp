#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ps2::gs::queue {

// kGif: arg0 the path, arg1 the qwords. kVif1: arg0 the words of VIF1 data (a transfer, or part of one: vif.cpp).
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

// GIF data added with add_gif() is sent once this much of it waits unsent: the reader gets it in pieces of
// about this size while the poster is busy, and the rest when the poster sends anything else or runs dry.
inline constexpr std::uint64_t kSendBytes = 128u << 10;

class Ring {
public:
    void start(std::size_t capacity);   // a multiple of 16
    bool active() const { return capacity_ != 0; }
    // Sends a message: when this returns the reader can take it and everything added before it. Waits for
    // room. A message larger than the whole ring is reported and dropped: no wait would end.
    void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
    // GIF data of one path (whole qwords), written into the ring but not sent yet. Data of the same path that
    // follows unsent data of that path joins its message: the reader walks a path's stream across messages
    // anyway (gif.cpp), so what it does is the same, with one header, one wake and one turn of its loop for
    // many pieces. Sent by the next post() or send(), or when kSendBytes wait. For a poster with a point where
    // it is sure to call send() before it sleeps: the VU thread, the only poster of the render ring while it
    // runs (system.cpp). With PS2_RING_EACH set every piece is a message of its own, sent at once, as before.
    void add_gif(std::uint32_t path, const void *data, std::uint32_t bytes);
    void send();   // the reader can take everything written so far, and is woken if it waits
    bool peek(Header &header, const std::uint8_t *&payload);
    void pop(const Header &header);
    // Sleeps until something is sent or the time runs out; returns at once when there is something to read.
    // Returns the milliseconds it slept, 0 when it did not, so that a caller counting its idle time needs no
    // clock of its own.
    double wait_for_work(int timeout_ms);
    void wake();   // ends a wait_for_work() early; any thread may call it
    std::uint64_t capacity() const { return capacity_; }
    void describe(const char *name) const;   // one line to stderr: positions and who is waiting
    // most bytes waiting at once, against the reader's position as the poster last read it (at each send)
    std::uint64_t high_water() const { return high_water_.load(std::memory_order_relaxed); }
    std::uint64_t largest() const { return largest_.load(std::memory_order_relaxed); }         // largest message, with its header
    std::uint64_t waits() const { return waits_.load(std::memory_order_relaxed); }             // times the poster found no room
    std::uint64_t lost_wakes() const { return lost_wakes_.load(); }
    std::uint64_t messages() const { return messages_.load(std::memory_order_relaxed); }     // messages written
    std::uint64_t sends() const { return sends_.load(std::memory_order_relaxed); }           // times the reader was given more
    std::uint64_t wakes() const { return wakes_.load(std::memory_order_relaxed); }           // times a send found the reader waiting

private:
    std::uint64_t reserve(std::uint64_t need);   // where a message of `need` bytes with its header goes; waits for room
    void write_header(std::uint64_t at, const Header &h) const;

    std::unique_ptr<std::uint8_t[]> buffer_;
    std::uint64_t capacity_ = 0;
    bool send_each_ = false;
    // The poster's own. write_ runs ahead of head_ by what is written and not sent yet; tail_seen_ is the
    // reader's tail as last read, so that a poster with room does not read the reader's line each time.
    alignas(64) std::uint64_t write_ = 0, tail_seen_ = 0, unsent_ = 0;
    std::uint64_t open_ = ~0ull;   // the header of the unsent GIF message data may join, if any
    std::uint32_t open_path_ = 0, open_bytes_ = 0;
    std::atomic<std::uint64_t> high_water_{0}, largest_{0}, waits_{0}, messages_{0}, sends_{0}, wakes_{0};   // written by the poster only
    alignas(64) std::atomic<std::uint64_t> head_{0};   // what the reader may take: up to here
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    alignas(64) std::atomic<std::uint32_t> freed_{0};
    std::atomic<std::uint32_t> producer_waiting_{0};
    std::atomic<std::uint64_t> lost_wakes_{0};   // waits for room that ran out their time although room had been freed
    // 1 while the reader is in wait_for_work(): it sleeps on this word, and a send that finds it 1 sets it
    // to 0 and wakes it. A reader that is woken, or that sees the 0 before it sleeps, cannot sleep through a
    // send, wherever the two threads interleave.
    alignas(64) std::atomic<std::uint32_t> consumer_waiting_{0};
};

Ring &render();
Ring &vu();
Role &role();
Ring &outbox();

bool active();
bool vu_active();
// A message to the next thread on the way to the GS (outbox()). GIF data from the VU thread is added to the
// render ring without being sent (Ring::add_gif); the VU thread sends it before it waits for work.
void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
void describe();   // both rings, see Ring::describe
void stats();   // how full each ring has been, to stderr (in the page: Module._ps2_ring_stats() from the console)

void wait(std::atomic<std::uint32_t> &value, std::uint32_t seen);
void notify(std::atomic<std::uint32_t> &value);

}
