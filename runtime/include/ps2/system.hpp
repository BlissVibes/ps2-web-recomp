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
    std::function<void(const std::string &name, const std::uint8_t *code, std::size_t size)> on_vu_program;
    std::string memcard_path;
    std::function<void()> on_memcard_saved;
    std::string save_state_path;
    std::uint64_t save_state_frame = 0;
    std::uint64_t snapshot_every = 0;   // save a state every N frames into snapshot_dir
    std::string snapshot_dir;
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
std::uint64_t hblank_cycles();
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
bool save_state(const std::string &path);
bool load_state(const std::string &path);
bool load_state(const std::uint8_t *data, std::size_t size);
bool halted();
std::uint64_t frame_count();
std::uint32_t state_version();
std::uint64_t preempt_count();
void describe_threads();   // what each thread may be waiting for, to stderr   // how often generated code has stopped to run due events

// An idle call: a game that waits in a loop, calling `function` through a pointer on every turn from the
// call that returns to `return_address`, with register `waiting_register` non-zero while it still waits.
// Nothing such a loop waits for can happen before the next event, and its turns are all alike: once two
// turns in a row have cost the same number of cycles, the runtime takes whole turns off the time slice
// at once and leaves the last one or two to run. The slice then ends exactly where it would have ended
// with the loop spinning, so nothing the game can observe changes. The function must do the same thing
// on every turn of one wait (the registration is that promise). Registered per game (the hooks table
// does it). PS2_NO_IDLE=1 turns it off.
void add_idle_call(std::uint32_t function, std::uint32_t return_address, int waiting_register);
std::uint64_t idle_calls_taken();   // turns skipped so far
void forget_idle_progress();        // start measuring afresh (tests)

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
void init_graphics();
void start_render_thread();
void start_vu_thread();
void run_vu_thread();
double vu_thread_idle_ms();
std::uint64_t vu1_interpreted_calls();
void post_present();
enum class Rendered { idle, presented, busy };
Rendered render_pending(bool surface, const std::function<void(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height)> &copy,
                        const std::function<void()> &before_present, double budget_ms);
double wait_render_work(int timeout_ms);   // the milliseconds it waited; 0 when there was work at once
void wake_render_thread();
bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool parse_press(const std::string &spec, Press &press);
double gpu_busy_ms();
void browser_yield();
std::uint64_t frames_run();
std::uint64_t primitives_drawn();
void set_pad_buttons(std::uint16_t pressed);
void set_pad_axis(int axis, std::uint8_t value);
void set_audio_sink(std::function<void(const std::int16_t *samples, std::size_t frames)> sink);

}
