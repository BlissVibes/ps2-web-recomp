#pragma once

#include "cpu.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2 {

inline constexpr std::uint64_t kEeClock = 294912000ull;
inline constexpr std::uint32_t kHandlerReturn = 0x00001000u;
inline constexpr std::uint32_t kIdle = 0x00001010u;

extern Cpu g_cpu;

struct Press {
    std::uint64_t frame;
    std::uint32_t frames;
    std::uint16_t buttons;
};

struct Options {
    bool trace_syscalls = false;
    bool trace_io = false;
    bool trace_threads = false;
    bool pal = true;
    bool rate_forced = false;
    std::uint64_t max_frames = 0;
    std::string misses_path;
    std::string disc_path;
    bool trace_iop_modules = false;
    bool trace_iop_hle = false;
    std::string frames_dir;
    std::uint32_t frame_every = 60;
    std::vector<Press> presses;
    std::function<void()> on_vblank;
};

extern Options g_options;

struct ThreadContext {
    Reg128 r[32];
    Reg128 hi, lo;
    std::uint32_t sa, pc;
    std::uint32_t f[32];
    std::uint32_t facc, fcr31;
};

void save_context(const Cpu &c, ThreadContext &t);
void load_context(Cpu &c, const ThreadContext &t);

std::uint64_t now();
void set_budget(Cpu &c, std::int64_t cycles);
void schedule(int event, std::uint64_t at, std::function<void()> action);
bool preempt(Cpu &c);

bool has_code(std::uint32_t pc);
void raise_intc(unsigned cause);
void request_reschedule();
void request_iop_sync();
void sync_iop();

bool load_elf(const std::string &path, std::uint32_t &entry);
void boot(std::uint32_t entry, const std::vector<std::string> &args);
int run();
void halt(const std::string &reason);
bool halted();
std::uint64_t frame_count();

bool display_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);

struct WindowTarget {
    enum Kind { X11, Wayland, Canvas } kind = X11;
    void *display = nullptr;
    std::uint64_t window = 0;
    void *surface = nullptr;
    const char *selector = nullptr;
    std::uint32_t width = 0, height = 0;
};

// Lets the WebGPU renderer present straight to a window or canvas instead of handing frames back.
bool attach_window(const WindowTarget &target);
void resize_window(std::uint32_t width, std::uint32_t height);
bool present_frame();
// Parses "frame:button[+button][:frames]" as used by --press.
bool parse_press(const std::string &spec, Press &press);
// GPU time spent on emulation so far, estimated from timestamp queries (0 when unsupported).
double gpu_busy_ms();
// Returns to the browser event loop once without the 4 ms clamp of nested timers (Emscripten only).
void browser_yield();
std::uint64_t frames_run();
std::uint64_t primitives_drawn();
void set_pad_buttons(std::uint16_t pressed);
void set_pad_axis(int axis, std::uint8_t value);
void set_audio_sink(std::function<void(const std::int16_t *samples, std::size_t frames)> sink);

}
