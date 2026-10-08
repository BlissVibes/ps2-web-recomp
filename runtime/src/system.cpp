#include "dmac.hpp"
#include "gs/gif.hpp"
#include "vif.hpp"
#include "gs/gs.hpp"
#include "hw.hpp"
#include "iop/iop.hpp"
#include "iop/sif.hpp"
#include "iop/sio2.hpp"
#include "iop/spu2.hpp"
#include "ipu/ipu.hpp"
#include "ps2/disc.hpp"
#include "kernel.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <iterator>

namespace ps2 {

Cpu g_cpu;
Options g_options;

namespace {

constexpr std::int64_t kMaxSlice = 20000;

struct Event {
    int id;
    std::uint64_t at;
    std::function<void()> action;
};

std::uint64_t g_base = 0;
std::int64_t g_given = 0;
std::vector<Event> g_events;
bool g_halted = false;
std::string g_halt_reason;
std::uint64_t g_frames = 0;
std::uint64_t g_iop_synced = 0;

std::uint64_t next_event_time() {
    std::uint64_t t = UINT64_MAX;
    for (const Event &e : g_events) t = std::min(t, e.at);
    return t;
}

void run_due_events() {
    sync_iop();
    for (;;) {
        const std::uint64_t t = now();
        auto it = std::min_element(g_events.begin(), g_events.end(), [](const Event &a, const Event &b) { return a.at < b.at; });
        if (it == g_events.end() || it->at > t) return;
        auto action = std::move(it->action);
        g_events.erase(it);
        action();
    }
}

void refill(Cpu &c) {
    const std::uint64_t t = now(), next = next_event_time();
    std::int64_t slice = kMaxSlice;
    if (next != UINT64_MAX) slice = std::clamp<std::int64_t>(static_cast<std::int64_t>(next - std::min(next, t)), 1, kMaxSlice);
    set_budget(c, slice);
}

std::uint64_t frame_cycles() { return kEeClock / (g_options.pal ? 50u : 60u); }

void dump_frame() {
    std::vector<std::uint32_t> rgba;
    std::uint32_t w = 0, h = 0;
    if (!gs::display(rgba, w, h)) return;
    char name[512];
    std::snprintf(name, sizeof name, "%s/frame_%05llu.ppm", g_options.frames_dir.c_str(), static_cast<unsigned long long>(g_frames));
    std::FILE *f = std::fopen(name, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::uint32_t p : rgba) {
        const std::uint8_t px[3] = {static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(p >> 8), static_cast<std::uint8_t>(p >> 16)};
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
}

void apply_presses() {
    if (g_options.presses.empty()) return;
    std::uint16_t mask = 0;
    for (const Press &p : g_options.presses)
        if (g_frames >= p.frame && g_frames < p.frame + p.frames) mask |= p.buttons;
    iop::sio2::set_buttons(0, mask);
}

void vblank() {
    ++g_frames;
    apply_presses();
    if (g_options.on_vblank) g_options.on_vblank();
    if (g_options.trace_iop_modules && g_frames % 25 == 0) std::fprintf(stderr, "[frame] %llu prims %llu\n", static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(gs::primitives_drawn()));
    if (!g_options.frames_dir.empty() && g_options.frame_every && g_frames % g_options.frame_every == 0) dump_frame();
    hw::vblank_start();
    iop::vblank(true);
    schedule(hw::kEventVblankEnd + 100, now() + 18874u * 22u, [] { iop::vblank(false); });
    kernel::on_vblank_start();
    if (g_options.max_frames && g_frames >= g_options.max_frames) halt("frame limit reached");
    schedule(hw::kEventVblank, now() + frame_cycles(), vblank);
}

void idle(Cpu &c) {
    std::uint64_t next = next_event_time();
    if (next == UINT64_MAX && iop::idle()) {
        halt("deadlock: every thread waits and no event is pending");
        return;
    }
    next = std::min(next, now() + kEeClock / 2000);
    g_base = std::max(now(), next);
    g_given = 0;
    c.budget = 0;
    run_due_events();
}

}

void save_context(const Cpu &c, ThreadContext &t) {
    std::memcpy(t.r, c.r, sizeof t.r);
    t.hi = c.hi;
    t.lo = c.lo;
    t.sa = c.sa;
    t.pc = c.pc & ~kUnwind;
    std::memcpy(t.f, c.f, sizeof t.f);
    t.facc = c.facc;
    t.fcr31 = c.fcr31;
}

void load_context(Cpu &c, const ThreadContext &t) {
    std::memcpy(c.r, t.r, sizeof t.r);
    c.hi = t.hi;
    c.lo = t.lo;
    c.sa = t.sa;
    c.pc = t.pc;
    std::memcpy(c.f, t.f, sizeof t.f);
    c.facc = t.facc;
    c.fcr31 = t.fcr31;
}

std::uint64_t now() { return g_base + static_cast<std::uint64_t>(g_given - g_cpu.budget); }

void set_budget(Cpu &c, std::int64_t cycles) {
    g_base = now();
    g_given = cycles;
    c.budget = static_cast<std::int32_t>(cycles);
}

void schedule(int event, std::uint64_t at, std::function<void()> action) {
    g_events.erase(std::remove_if(g_events.begin(), g_events.end(), [&](const Event &e) { return e.id == event; }), g_events.end());
    g_events.push_back({event, at, std::move(action)});
}

bool preempt(Cpu &c) {
    run_due_events();
    refill(c);
    return g_halted || kernel::must_unwind();
}

void halt(const std::string &reason) {
    if (!g_halted) std::fprintf(stderr, "[ee] halted: %s\n", reason.c_str());
    g_halted = true;
    g_halt_reason = reason;
}

bool halted() { return g_halted; }

std::uint64_t frame_count() { return g_frames; }

bool load_elf(const std::string &path, std::uint32_t &entry) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(file), {}};
    if (data.size() < 52 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) return false;
    const auto u32 = [&](std::size_t o) { std::uint32_t v; std::memcpy(&v, data.data() + o, 4); return v; };
    const auto u16 = [&](std::size_t o) { std::uint16_t v; std::memcpy(&v, data.data() + o, 2); return v; };
    entry = u32(24);
    const std::uint32_t phoff = u32(28);
    for (std::uint32_t i = 0; i < u16(44); ++i) {
        const std::size_t ph = phoff + i * u16(42);
        if (u32(ph) != 1) continue;
        const std::uint32_t offset = u32(ph + 4), vaddr = physical(u32(ph + 8)), filesz = u32(ph + 16), memsz = u32(ph + 20);
        if (vaddr + memsz > kRamSize || offset + filesz > data.size()) return false;
        std::memcpy(g_ram + vaddr, data.data() + offset, filesz);
        std::memset(g_ram + vaddr + filesz, 0, memsz - filesz);
    }
    return true;
}

void sync_iop() {
    const std::uint64_t t = now();
    if (t > g_iop_synced) {
        iop::run(t - g_iop_synced);
        g_iop_synced = t;
    }
    sif::pump();
}

void request_iop_sync() {
    if (g_cpu.budget > 64) set_budget(g_cpu, 64);
}

void boot(std::uint32_t entry, const std::vector<std::string> &args) {
    hw::reset();
    kernel::reset();
    if (!g_options.disc_path.empty() && !disc::open(g_options.disc_path))
        std::fprintf(stderr, "[ee] cannot open disc image %s\n", g_options.disc_path.c_str());
    iop::set_trace(g_options.trace_iop_modules, g_options.trace_iop_hle);
    iop::reset();
    std::uint32_t argv = 0x00000800u, payload = argv + 4u * static_cast<std::uint32_t>(args.size() + 1);
    for (std::size_t i = 0; i < args.size(); ++i) {
        store<std::uint32_t>(g_ram + argv + 4 * i, payload);
        std::memcpy(g_ram + payload, args[i].c_str(), args[i].size() + 1);
        payload += static_cast<std::uint32_t>(args[i].size() + 1);
    }
    store<std::uint32_t>(g_ram + argv + 4 * args.size(), 0);
    kernel::start_main_thread(entry, 0, static_cast<std::uint32_t>(args.size()), argv);
    schedule(hw::kEventVblank, frame_cycles(), vblank);
}

int run() {
    Cpu &c = g_cpu;
    set_budget(c, kMaxSlice);
    while (!g_halted) {
        c.pc &= ~kUnwind;
        run_due_events();
        kernel::service(c);
        if (g_halted) break;
        if (c.pc == kIdle) {
            idle(c);
            continue;
        }
        if (c.budget <= 0) refill(c);
        dispatch(c, 0, 0);
    }
    kernel::dump();
    iop::dump();
    sif::dump();
    if (const char *peek = std::getenv("PS2_PEEK")) {
        std::uint32_t at = 0, len = 0;
        if (std::sscanf(peek, "%x,%x", &at, &len) == 2)
            for (std::uint32_t i = 0; i < len; i += 16) {
                std::fprintf(stderr, "[peek] %08X:", at + i);
                for (std::uint32_t j = 0; j < 16 && i + j < len; j += 4) std::fprintf(stderr, " %08X", ld32(at + i + j));
                std::fprintf(stderr, "\n");
            }
    }
    if (const char *path = std::getenv("PS2_DUMP")) {
        if (std::FILE *f = std::fopen(path, "wb")) {
            std::fwrite(g_ram, 1, kRamSize, f);
            std::fwrite(iop::ram_ptr(0), 1, iop::kRamSize, f);
            std::fwrite(iop::spu2::ram(), 2, 1u << 20, f);
            std::fclose(f);
        }
    }
    gs::stats();
    ipu::stats();
    if (const char *path = std::getenv("PS2_DUMP_AUDIO")) iop::spu2::dump_audio(path);
    if (std::getenv("PS2_TRACE_SPU2")) iop::spu2::dump_voices();
    dmac::stats();
    gif::stats();
    vif::stats();
    std::fprintf(stderr, "[ee] stopped after %llu frames, %llu cycles: %s\n",
                 static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(now()), g_halt_reason.c_str());
    return 0;
}

bool display_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return gs::display(rgba, width, height); }
bool attach_window(const WindowTarget &t) { return gs::attach_window(static_cast<int>(t.kind), t.display, t.window, t.surface, t.selector, t.width, t.height); }
void resize_window(std::uint32_t width, std::uint32_t height) { gs::resize_window(width, height); }
bool present_frame() { return gs::present(); }
double gpu_busy_ms() { return gs::gpu_busy_ms(); }
void set_pad_buttons(std::uint16_t pressed) { iop::sio2::set_buttons(0, pressed); }
void set_pad_axis(int axis, std::uint8_t value) { iop::sio2::set_axis(0, axis, value); }
void set_audio_sink(std::function<void(const std::int16_t *, std::size_t)> sink) { iop::spu2::set_sink(std::move(sink)); }

}
