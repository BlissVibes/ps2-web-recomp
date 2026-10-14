#include "disc_cache.hpp"
#include "pace.hpp"
#include "sha256.hpp"

#include "ps2/disc.hpp"
#include "ps2/system.hpp"

#include <emscripten.h>
#include <emscripten/stack.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void register_generated_code();
void register_generated_vu();
void register_hooks();

EM_JS_DEPS(ps2web, "$stringToNewUTF8,$UTF8ToString");

EM_JS(void, ps2web_fetch, (const char *url, double offset, int size, void *out, std::atomic<std::uint32_t> *state), {
    const finish = (value) => {
        Atomics.store(HEAP32, state >> 2, value);
        Atomics.notify(HEAP32, state >> 2);
    };
    const store = (buffer, from) => {
        let bytes = new Uint8Array(buffer);
        if (from) bytes = bytes.subarray(from, from + size);
        HEAPU8.set(bytes.subarray(0, Math.min(bytes.length, size)), out);
        finish(3);
    };
    const fail = (e) => {
        console.warn('disc read failed', e);
        finish(4);
    };
    if (Module.discFile) {
        Module.discFile.slice(offset, offset + size).arrayBuffer().then((buffer) => store(buffer, 0)).catch(fail);
        return;
    }
    const target = UTF8ToString(url);
    fetch(target, {headers: {Range: 'bytes=' + offset + '-' + (offset + size - 1)}})
        .then((response) => {
            if (response.status !== 206 && response.status !== 200) throw new Error('status ' + response.status);
            return response.arrayBuffer().then((buffer) => store(buffer, response.status === 200 ? offset : 0));
        })
        .catch(fail);
});

EM_JS(int, ps2web_save_program, (const char *name, const std::uint8_t *code, int size), {
    try {
        const xhr = new XMLHttpRequest();
        xhr.open('PUT', 'vu1/' + UTF8ToString(name), false);
        xhr.send(HEAPU8.slice(code, code + size));
        return xhr.status;
    } catch (e) {
        return 0;
    }
});

EM_JS(double, ps2web_disc_size, (), { return Module.discSize || 0; });
EM_JS(char *, ps2web_state_url, (), { return Module.stateUrl ? stringToNewUTF8(new URL(Module.stateUrl, location.href).href) : 0; });
EM_JS(double, ps2web_url_size, (const char *url), {
    try {
        const xhr = new XMLHttpRequest();
        xhr.open('HEAD', UTF8ToString(url), false);
        xhr.send();
        return xhr.status === 200 ? Number(xhr.getResponseHeader('Content-Length') || 0) : 0;
    } catch (e) {
        return 0;
    }
});
EM_JS(double, ps2web_url_read, (const char *url, std::uint8_t *out, double size), {
    try {
        const xhr = new XMLHttpRequest();
        xhr.open('GET', UTF8ToString(url), false);
        xhr.responseType = 'arraybuffer';
        xhr.send();
        if (xhr.status !== 200) return 0;
        const bytes = new Uint8Array(xhr.response);
        HEAPU8.set(bytes.subarray(0, size), out);
        return bytes.length;
    } catch (e) {
        return 0;
    }
});
EM_JS(char *, ps2web_disc_url, (), { return stringToNewUTF8(new URL(Module.discUrl, location.href).href); });
EM_JS(void, ps2web_frame_done, (), {
    if (!Module.shownChannel) {
        Module.shownChannel = new MessageChannel();
        Module.shownWaiters = [];
        Module.shownChannel.port1.onmessage = () => {
            Module.shownPending = false;
            const waiters = Module.shownWaiters;
            Module.shownWaiters = [];
            for (const resolve of waiters) resolve();
        };
    }
    Module.shownPending = true;
    requestAnimationFrame(() => Module.shownChannel.port2.postMessage(0));
    if (Module.onFrame) Module.onFrame();
});
// Waits until the display has shown the last frame presented, for `limit_ms` at the most (0: not at all,
// Infinity: however long). 1 when it has been shown, 0 when the wait was given up.
EM_ASYNC_JS(int, ps2web_wait_shown, (double limit_ms), {
    if (!Module.shownPending) return 1;
    if (!(limit_ms > 0)) return 0;
    if (limit_ms === Infinity) {
        await new Promise((resolve) => Module.shownWaiters.push(resolve));
        return 1;
    }
    let timer = 0;
    const shown = await new Promise((resolve) => {
        Module.shownWaiters.push(() => resolve(1));
        timer = setTimeout(() => resolve(0), Math.ceil(limit_ms));
    });
    clearTimeout(timer);
    return shown;
});
EM_JS(void, ps2web_blit, (const std::uint32_t *rgba, int width, int height), {
    const canvas = Module.canvas;
    if (!Module.blitContext) Module.blitContext = canvas.getContext('2d');
    if (!Module.blitImage || Module.blitImage.width !== width || Module.blitImage.height !== height) {
        Module.blitImage = new ImageData(width, height);
        Module.blitSource = new OffscreenCanvas(width, height);
    }
    Module.blitImage.data.set(HEAPU8.subarray(rgba, rgba + width * height * 4));
    Module.blitSource.getContext('2d').putImageData(Module.blitImage, 0, 0);
    const ctx = Module.blitContext;
    const scale = Math.min(canvas.width / 4, canvas.height / 3);
    const w = scale * 4, h = scale * 3;
    const sx = Math.max(1, Math.floor(w / width)), sy = Math.max(1, Math.floor(h / height));
    let source = Module.blitSource;
    if (sx > 1 || sy > 1) {
        if (!Module.blitSharp || Module.blitSharp.width !== width * sx || Module.blitSharp.height !== height * sy)
            Module.blitSharp = new OffscreenCanvas(width * sx, height * sy);
        const sharp = Module.blitSharp.getContext('2d');
        sharp.imageSmoothingEnabled = false;
        sharp.drawImage(Module.blitSource, 0, 0, width * sx, height * sy);
        source = Module.blitSharp;
    }
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(source, (canvas.width - w) / 2, (canvas.height - h) / 2, w, h);
});

namespace {

using ps2web::kChunk, ps2web::kWanted, ps2web::kLoading, ps2web::kReady, ps2web::kFailed, ps2web::Slot;

constexpr std::uint32_t kAudioFrames = 1u << 14;

struct AudioRing {
    std::atomic<std::uint32_t> write{0}, read{0};
    std::uint32_t capacity = kAudioFrames;
    std::atomic<std::uint32_t> active{0};
    std::int16_t samples[kAudioFrames * 2];
};

AudioRing g_audio;
std::atomic<const char *> g_emu_waits{"nothing"};   // what the emulation thread is waiting for, for ps2web_describe
void mark_sleep(useconds_t us) {
    struct M { const char *was = g_emu_waits.exchange("a sleep in pace, the audio wait or a disc retry"); ~M() { g_emu_waits.store(was); } } m;
    usleep(us);
}

struct Waiting {
    const char *was;
    explicit Waiting(const char *what) : was(g_emu_waits.exchange(what)) {}
    ~Waiting() { g_emu_waits.store(was); }
};
ps2web::DiscCache g_cache;
std::uint64_t &g_disc_size = g_cache.disc_size;
char *g_disc_url = nullptr;
char *g_state_url = nullptr;
// Module.paceOld (the page's ?pace=old): wait as the page did before PresentGate and AudioStall, to compare the two.
bool g_old_pacing = false;
// Where the time of each thread goes, in ms. The emulation thread is idle while it sleeps in pace(), held
// while post_present() stops it until frames it has posted are drawn, and reads the disc in read_disc().
// Only the idle part is time to spare.
std::atomic<double> g_emulation_idle{0.0}, g_emulation_held{0.0}, g_disc_wait{0.0}, g_render_idle{0.0};

const Slot *chunk(std::uint64_t index) {
    Slot *s = g_cache.read(index);
    if (g_cache.asked) {
        g_cache.asked = false;
        ps2::wake_render_thread();
    }
    // With every slot wanted or loading there is none to give up: wait for a fetch to end (up to five
    // seconds) rather than fail the read, which the disc layer would fill with zeros.
    for (int waited = 0; !s && index * kChunk < g_disc_size && waited < 5000; ++waited) {
        mark_sleep(1000);
        s = g_cache.want(index);
        if (g_cache.asked) {
            g_cache.asked = false;
            ps2::wake_render_thread();
        }
    }
    for (int attempt = 0; s && attempt < 4; ++attempt) {
        for (std::uint32_t state = s->state.load(); state != kReady && state != kFailed; state = s->state.load()) {
            Waiting mark("a disc chunk to arrive");
            emscripten_futex_wait(&s->state, state, INFINITY);
        }
        if (s->state.load() == kReady) {
            if (s->index != index) break;   // cannot happen (disc_cache keeps the slot being read); never hand over another chunk's bytes
            return s;
        }
        mark_sleep(100000);
        s->state.store(kWanted, std::memory_order_release);
        ps2::wake_render_thread();
    }
    return nullptr;
}

std::size_t read_disc(std::uint64_t offset, void *out, std::size_t size) {
    std::size_t done = 0;
    auto *to = static_cast<std::uint8_t *>(out);
    while (done < size && offset + done < g_disc_size) {
        const std::uint64_t at = offset + done;
        const double asked = emscripten_get_now();
        const Slot *c = chunk(at / kChunk);
        g_disc_wait.store(g_disc_wait.load() + emscripten_get_now() - asked);
        if (!c) break;
        const std::size_t within = static_cast<std::size_t>(at % kChunk);
        const std::size_t take = std::min<std::size_t>(size - done, c->size - within);
        std::memcpy(to + done, c->data + within, take);
        done += take;
    }
    return done;
}

void service_disc() {
    for (Slot &s : g_cache.slots) {
        std::uint32_t expected = kWanted;
        if (s.state.compare_exchange_strong(expected, kLoading, std::memory_order_acq_rel))
            ps2web_fetch(g_disc_url, static_cast<double>(s.index * kChunk), static_cast<int>(s.size), s.data, &s.state);
    }
}

bool g_audio_stopped = false;   // the device has stopped taking samples (pace() noticed); emulation thread only

void push_audio(const std::int16_t *samples, std::size_t frames) {
    // A stopped device empties nothing: sound added now would only be played late when it starts again.
    if (g_audio_stopped) {
        if (!g_audio.active.load()) return;
        g_audio_stopped = false;
    }
    std::uint32_t write = g_audio.write.load(std::memory_order_relaxed);
    const std::uint32_t read = g_audio.read.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < frames && write - read < kAudioFrames; ++i, ++write) {
        const std::uint32_t at = (write & (kAudioFrames - 1)) * 2;
        g_audio.samples[at] = samples[i * 2];
        g_audio.samples[at + 1] = samples[i * 2 + 1];
    }
    g_audio.write.store(write, std::memory_order_release);
}

double audio_queued_ms() { return static_cast<double>(g_audio.write.load() - g_audio.read.load()) / 48.0; }

double g_next_frame = 0.0;

double frame_ms() { return ps2::g_options.pal ? 20.0 : 1000.0 / 59.94; }

void pace() {
    if (g_audio.active.load()) {
        // The audio device sets the pace: sleep while more than 80 ms of sound is queued.
        ps2web::AudioStall stall(g_audio.read.load());
        bool stopped = false;
        while (audio_queued_ms() > 80.0 && !stopped) {
            mark_sleep(1000);
            stopped = !g_old_pacing && stall.stopped(g_audio.read.load());
        }
        if (!stopped) {
            g_next_frame = 0.0;
            return;
        }
        // Nothing has taken a sample for 200 ms. The clock sets the pace until the worklet marks the
        // ring active again, which it does every time it runs.
        g_audio.active.store(0);
        g_audio_stopped = true;
        std::fprintf(stderr, "[web] the audio device has stopped taking samples: pacing by the clock\n");
    }
    const double period = frame_ms();
    const double now = emscripten_get_now();
    if (g_next_frame == 0.0 || now - g_next_frame > 200.0) g_next_frame = now;
    g_next_frame += period;
    if (g_next_frame > now) mark_sleep(static_cast<useconds_t>((g_next_frame - now) * 1000.0));
}

void on_vblank() {
    const double start = emscripten_get_now();
    ps2::post_present();
    const double posted = emscripten_get_now();
    pace();
    const double end = emscripten_get_now();
    g_emulation_held.store(g_emulation_held.load() + posted - start);
    g_emulation_idle.store(g_emulation_idle.load() + end - posted);
}

// Where the stack of each thread lies, for ps2web_stack_used: emulation, main, VU (the order of ps2web_idle_ms).
struct Stack {
    std::atomic<std::uintptr_t> base{0}, end{0};
};
Stack g_stacks[3];
// Stack sizes of the two threads made here (the main thread's is -sSTACK_SIZE in CMakeLists.txt, 1 MB).
// Measured with ps2web_stack_used after boot, menus, video and a race (docs/re/web-log-005.md in the
// Burnout 3 repo): emulation 53,736 bytes, main 2,208, VU 18,208. The VU thread runs one flat loop (the
// largest frame on it is XGKICK's, 16 KB), so 1 MB is more than fifty times what it uses. The emulation
// thread runs the game's own calls: generated code nests up to kMaxDepth (192) calls before it unwinds to
// the dispatch loop, an interrupt handler or a game syscall handler starts another such loop on top
// (run_guest, at most three deep), and the largest frame of a generated function is about 11.6 KB, so a
// game that recursed that deep would need about 6.7 MB. 8 MB covers that; the race uses 1/150 of it.
constexpr std::size_t kEmulationStack = 8u << 20, kVuStack = 1u << 20;

void note_stack(int which) {
    g_stacks[which].base.store(emscripten_stack_get_base());
    g_stacks[which].end.store(emscripten_stack_get_end());
}

// The path the game boots from, as /game/boot.txt gives it (the disc's own name for its executable).
std::string boot_program() {
    std::string boot = "/game/core.elf";
    if (std::FILE *f = std::fopen("/game/boot.txt", "rb")) {
        char text[256] = {};
        const std::size_t n = std::fread(text, 1, sizeof text - 1, f);
        std::fclose(f);
        std::string line(text, n);
        line.erase(line.find_last_not_of(" \r\n\t") + 1);
        if (!line.empty()) boot = line;
    }
    return boot;
}

#ifdef PS2_WEB_ELF_SHA256
bool refuse(const std::string &why) {
    std::fprintf(stderr, "[web] %s\n", why.c_str());
    MAIN_THREAD_EM_ASM({ if (Module.ps2Fatal) Module.ps2Fatal(UTF8ToString($0)); }, why.c_str());
    return false;
}

// A build with PS2_WEB_ELF_FROM_DISC leaves the executable out of the page's data. It is read from the
// user's disc into the place load_elf reads, and only if it is byte for byte the file the game's code was
// translated from: the page accepts a disc by the name of its boot program alone, and a disc read that
// comes back short is padded with zeros without an error.
bool executable_from_disc() {
    const std::string boot = boot_program();
    const auto file = ps2::disc::is_open() ? ps2::disc::find(boot) : std::nullopt;
    if (!file || file->directory) return refuse("The disc has no " + boot);
    const std::uint32_t sectors = (file->size + ps2::disc::kSector - 1) / ps2::disc::kSector;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(sectors) * ps2::disc::kSector);
    if (file->size != PS2_WEB_ELF_SIZE || !ps2::disc::read(file->sector, sectors, data.data()) ||
        ps2web::sha256_hex(data.data(), file->size) != PS2_WEB_ELF_SHA256)
        return refuse("The executable on this disc is not the one this page was built from");
    mkdir("/game", 0777);
    std::FILE *out = std::fopen("/game/core.elf", "wb");
    const bool stored = out && std::fwrite(data.data(), 1, file->size, out) == file->size;
    if (out) std::fclose(out);
    return stored || refuse("Cannot store the game executable");
}
#endif

struct Launch {
    std::vector<std::string> args;
};

void *emulate(void *data) {
    note_stack(0);
    const Launch &launch = *static_cast<const Launch *>(data);
    if (g_disc_size && !ps2::disc::open("disc", g_disc_size, read_disc)) std::fprintf(stderr, "[web] cannot read the disc image\n");
    std::uint32_t entry = 0;
#ifdef PS2_WEB_ELF_SHA256
    if (!executable_from_disc()) return nullptr;
#endif
    if (!ps2::load_elf("/game/core.elf", entry)) {
        std::fprintf(stderr, "cannot load the game executable\n");
        return nullptr;
    }
#ifdef PS2_WEB_ELF_SHA256
    std::remove("/game/core.elf");   // the copy is in the game's memory now
#endif
    ps2::boot(entry, launch.args);
    if (g_state_url) {
        const double size = ps2web_url_size(g_state_url);
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        if (size <= 0 || ps2web_url_read(g_state_url, data.data(), size) != size || !ps2::load_state(data.data(), data.size())) {
            std::fprintf(stderr, "[web] cannot load the state %s\n", g_state_url);
            return nullptr;
        }
    }
    ps2::run();
    return nullptr;
}

}

extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_pad(int buttons, int lx, int ly, int rx, int ry) {
    ps2::set_pad_buttons(static_cast<std::uint16_t>(buttons));
    ps2::set_pad_axis(0, static_cast<std::uint8_t>(rx));
    ps2::set_pad_axis(1, static_cast<std::uint8_t>(ry));
    ps2::set_pad_axis(2, static_cast<std::uint8_t>(lx));
    ps2::set_pad_axis(3, static_cast<std::uint8_t>(ly));
}

extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_resize(int width, int height) {
    ps2::resize_window(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
}

extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_gpu_ms() { return ps2::gpu_busy_ms(); }
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_frames() { return static_cast<double>(ps2::frames_run()); }
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_primitives() { return static_cast<double>(ps2::primitives_drawn()); }
extern "C" EMSCRIPTEN_KEEPALIVE void *ps2web_audio_ring() { return &g_audio; }
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_audio_queued_ms() { return audio_queued_ms(); }
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_vu_interpreted() { return static_cast<double>(ps2::vu1_interpreted_calls()); }
// 0: the emulation thread asleep with time to spare, 1: the main thread waiting, 2: the VU thread waiting,
// 3: the emulation thread held until frames it has posted are drawn, 4: the emulation thread reading the disc.
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_idle_ms(int which) {
    if (which == 2) return ps2::vu_thread_idle_ms();
    if (which == 3) return g_emulation_held.load();
    if (which == 4) return g_disc_wait.load();
    return which ? g_render_idle.load() : g_emulation_idle.load();
}

// The most a thread's stack has held so far, in bytes (0 emulation, 1 main, 2 VU). A stack grows downwards
// through memory that was zeros when the thread started, so this is the distance from its base down to the
// lowest byte that is not zero. If the stack got memory something else had used and freed, the answer is
// too large, never too small. For measuring only: it reads the whole unused part of the stack.
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_stack_used(int which) {
    if (which < 0 || which > 2) return 0.0;
    const std::uintptr_t base = g_stacks[which].base.load(), end = g_stacks[which].end.load();
    std::uintptr_t at = (end + 7) & ~static_cast<std::uintptr_t>(7);
    while (at < base && *reinterpret_cast<const std::uint64_t *>(at) == 0) at += 8;
    return static_cast<double>(base - std::min(at, base));
}

// How far the page's memory has been used, in bytes: the end of everything ever allocated (it never moves
// back). The memory grows when this passes its size, so this is what -sINITIAL_MEMORY has to cover.
// For a page that has stopped: what each thread may be waiting for, to the console (Module._ps2web_describe()).
extern "C" EMSCRIPTEN_KEEPALIVE void ps2web_describe() {
    ps2::describe_threads();
    std::uint32_t wanted = 0, loading = 0, ready = 0, failed = 0;
    for (const Slot &s : g_cache.slots) {
        const std::uint32_t state = s.state.load();
        wanted += state == kWanted;
        loading += state == kLoading;
        ready += state == kReady;
        failed += state == kFailed;
    }
    std::fprintf(stderr, "[stuck] emulation thread waits for %s; disc slots wanted %u, loading %u, ready %u, failed %u; last chunk %llu\n", g_emu_waits.load(),
                 wanted, loading, ready, failed, static_cast<unsigned long long>(g_cache.last_chunk));
    std::fprintf(stderr, "[stuck] audio ring: %u frames queued, device taking samples %u, counted as stopped %d\n",
                 g_audio.write.load() - g_audio.read.load(), g_audio.active.load(), g_audio_stopped ? 1 : 0);
}

extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_heap_top() { return static_cast<double>(reinterpret_cast<std::uintptr_t>(sbrk(0))); }

int main(int argc, char **argv) {
    note_stack(1);
    g_old_pacing = EM_ASM_INT({ return Module.paceOld ? 1 : 0; });
    static Launch launch;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) launch.args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (ps2::parse_press(argv[++i], press)) ps2::g_options.presses.push_back(press);
        }
    }
    if (launch.args.empty()) launch.args.push_back(boot_program());
    g_disc_size = static_cast<std::uint64_t>(ps2web_disc_size());
    g_disc_url = ps2web_disc_url();
    g_state_url = ps2web_state_url();
    ps2::g_options.on_vblank = on_vblank;
    ps2::g_options.on_vu_program = [](const std::string &name, const std::uint8_t *code, std::size_t size) {
        static bool unsupported = false;
        if (unsupported) return;
        const int status = ps2web_save_program(name.c_str(), code, static_cast<int>(size));
        if (status == 200 || status == 201) std::fprintf(stderr, "[web] saved VU1 program %s\n", name.c_str());
        else unsupported = true;
    };
    if (!EM_ASM_INT({ return Module.noCard ? 1 : 0; })) {
        ps2::g_options.memcard_path = "/memcard/Mcd001.ps2";
        ps2::g_options.on_memcard_saved = [] { MAIN_THREAD_ASYNC_EM_ASM({ if (Module.ps2CardSaved) Module.ps2CardSaved(); }); };
    }
    ps2::set_audio_sink(push_audio);
    register_generated_code();
    register_generated_vu();
    register_hooks();

    ps2::init_graphics();
    const bool no_surface = EM_ASM_INT({ return Module.noSurface ? 1 : 0; });
    bool copy = EM_ASM_INT({ return Module.presentCopy ? 1 : 0; });
    bool surface = false;
    if (!copy && !no_surface) {
        ps2::WindowTarget target;
        target.kind = ps2::WindowTarget::Canvas;
        target.selector = "#canvas";
        target.width = static_cast<std::uint32_t>(EM_ASM_INT({ return Module.canvas.width; }));
        target.height = static_cast<std::uint32_t>(EM_ASM_INT({ return Module.canvas.height; }));
        surface = ps2::attach_window(target);
    }
    copy = copy || (!surface && !no_surface);
    std::fprintf(stderr, "[web] %s\n", surface ? "presenting through WebGPU" : copy ? "presenting through a 2D canvas copy" : "not presenting");

    ps2::start_render_thread();
    pthread_attr_t attr, vu_attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kEmulationStack);
    pthread_attr_init(&vu_attr);
    pthread_attr_setstacksize(&vu_attr, kVuStack);
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        ps2::start_vu_thread();
        pthread_t vu_thread;
        if (pthread_create(&vu_thread, &vu_attr, [](void *) -> void * {
                note_stack(2);
                ps2::run_vu_thread();
                return nullptr;
            }, nullptr) != 0) {
            std::fprintf(stderr, "[web] cannot start the VU thread\n");
            return 1;
        }
    }
    pthread_t thread;
    if (pthread_create(&thread, &attr, emulate, &launch) != 0) {
        std::fprintf(stderr, "[web] cannot start the emulation thread\n");
        return 1;
    }
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> blit = [](const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) {
        ps2web_blit(rgba, static_cast<int>(width), static_cast<int>(height));
    };
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> none;
    static ps2web::PresentGate gate;
    const std::function<void()> wait_shown = [] {
        static int since_skip = 0;
        static double window_start = 0.0, window_gpu = 0.0, gpu_load = 1.0;
        const double start = emscripten_get_now();
        if (start - window_start > 500.0) {
            const double gpu = ps2::gpu_busy_ms();
            gpu_load = window_start > 0.0 ? (gpu - window_gpu) / (start - window_start) : 1.0;
            window_start = start;
            window_gpu = gpu;
        }
        // A present goes ahead without the display only while the graphics card has time to spare.
        const bool spare = gpu_load < 0.8;
        if (++since_skip > 8 && spare && g_audio.active.load() && audio_queued_ms() < 40.0 &&
            (g_old_pacing || gate.unshown < ps2web::PresentGate::kMaxUnshown)) {
            since_skip = 0;
            gate.presented(start, false);
            return;
        }
        const double limit = spare && !g_old_pacing ? gate.wait_limit(start, frame_ms()) : ps2web::kUntilShown;
        const bool shown = ps2web_wait_shown(limit);
        const double end = emscripten_get_now();
        gate.presented(end, shown);
        g_render_idle.store(g_render_idle.load() + end - start);
    };
    for (;;) {
        service_disc();
        const ps2::Rendered rendered = ps2::render_pending(surface, copy ? blit : none, wait_shown, 6.0);
        if (rendered == ps2::Rendered::presented) ps2web_frame_done();
        if (rendered != ps2::Rendered::idle) {
            ps2::browser_yield();
        } else {
            gate.queue_empty();
            if (const double waited = ps2::wait_render_work(50)) g_render_idle.store(g_render_idle.load() + waited);
        }
    }
}
