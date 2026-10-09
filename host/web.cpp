#include "ps2/disc.hpp"
#include "ps2/system.hpp"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>
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

EM_JS_DEPS(ps2web, "$stringToNewUTF8,$UTF8ToString");

EM_JS(void, ps2web_fetch, (const char *url, double offset, int size, void *out, std::atomic<std::uint32_t> *state), {
    const target = UTF8ToString(url);
    const finish = (value) => {
        Atomics.store(HEAP32, state >> 2, value);
        Atomics.notify(HEAP32, state >> 2);
    };
    fetch(target, {headers: {Range: 'bytes=' + offset + '-' + (offset + size - 1)}})
        .then((response) => {
            if (response.status !== 206 && response.status !== 200) throw new Error('status ' + response.status);
            return response.arrayBuffer().then((buffer) => {
                let bytes = new Uint8Array(buffer);
                if (response.status === 200) bytes = bytes.subarray(offset, offset + size);
                HEAPU8.set(bytes.subarray(0, Math.min(bytes.length, size)), out);
                finish(3);
            });
        })
        .catch((e) => {
            console.warn('disc read failed', e);
            finish(4);
        });
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
EM_JS(char *, ps2web_disc_url, (), { return stringToNewUTF8(new URL(Module.discUrl, location.href).href); });
EM_JS(void, ps2web_frame_done, (), { if (Module.onFrame) Module.onFrame(); });
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
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(Module.blitSource, (canvas.width - w) / 2, (canvas.height - h) / 2, w, h);
});

namespace {

constexpr std::size_t kChunk = 256u << 10;
constexpr std::uint32_t kSlots = 256;
constexpr std::uint64_t kReadAhead = 16;
constexpr std::uint32_t kAudioFrames = 1u << 14;

enum : std::uint32_t { kEmpty, kWanted, kLoading, kReady, kFailed };

struct Slot {
    std::atomic<std::uint32_t> state{kEmpty};
    std::uint64_t index = 0, used = 0;
    std::uint32_t size = 0;
    std::uint8_t *data = nullptr;
};

struct AudioRing {
    std::atomic<std::uint32_t> write{0}, read{0};
    std::uint32_t capacity = kAudioFrames;
    std::atomic<std::uint32_t> active{0};
    std::int16_t samples[kAudioFrames * 2];
};

AudioRing g_audio;
Slot g_slots[kSlots];
std::uint64_t g_disc_size = 0, g_use_clock = 0, g_last_chunk = ~0ull, g_run = 0;
char *g_disc_url = nullptr;

Slot *find_slot(std::uint64_t index) {
    for (Slot &s : g_slots)
        if (s.state.load(std::memory_order_acquire) != kEmpty && s.index == index) return &s;
    return nullptr;
}

Slot *want(std::uint64_t index) {
    if (index * kChunk >= g_disc_size) return nullptr;
    if (Slot *s = find_slot(index)) return s;
    Slot *victim = nullptr;
    for (Slot &s : g_slots) {
        const std::uint32_t state = s.state.load(std::memory_order_acquire);
        if (state == kEmpty) {
            victim = &s;
            break;
        }
        if ((state == kReady || state == kFailed) && (!victim || s.used < victim->used)) victim = &s;
    }
    if (!victim) return nullptr;
    if (!victim->data) victim->data = new std::uint8_t[kChunk];
    victim->index = index;
    victim->size = static_cast<std::uint32_t>(std::min<std::uint64_t>(kChunk, g_disc_size - index * kChunk));
    victim->used = ++g_use_clock;
    victim->state.store(kWanted, std::memory_order_release);
    ps2::wake_render_thread();
    return victim;
}

const Slot *chunk(std::uint64_t index) {
    Slot *s = want(index);
    if (index == g_last_chunk + 1) ++g_run;
    else if (index != g_last_chunk) g_run = 0;
    g_last_chunk = index;
    for (std::uint64_t k = 1; k <= std::min(g_run * 2, kReadAhead); ++k) want(index + k);
    for (int attempt = 0; s && attempt < 4; ++attempt) {
        for (std::uint32_t state = s->state.load(); state != kReady && state != kFailed; state = s->state.load())
            emscripten_futex_wait(&s->state, state, INFINITY);
        if (s->state.load() == kReady) {
            s->used = ++g_use_clock;
            return s;
        }
        usleep(100000);
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
        const Slot *c = chunk(at / kChunk);
        if (!c) break;
        const std::size_t within = static_cast<std::size_t>(at % kChunk);
        const std::size_t take = std::min<std::size_t>(size - done, c->size - within);
        std::memcpy(to + done, c->data + within, take);
        done += take;
    }
    return done;
}

void service_disc() {
    for (Slot &s : g_slots) {
        std::uint32_t expected = kWanted;
        if (s.state.compare_exchange_strong(expected, kLoading, std::memory_order_acq_rel))
            ps2web_fetch(g_disc_url, static_cast<double>(s.index * kChunk), static_cast<int>(s.size), s.data, &s.state);
    }
}

void push_audio(const std::int16_t *samples, std::size_t frames) {
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
std::atomic<double> g_emulation_idle{0.0}, g_render_idle{0.0};

void pace() {
    ps2::post_present();
    if (g_audio.active.load()) {
        while (audio_queued_ms() > 80.0) usleep(1000);
        g_next_frame = 0.0;
        return;
    }
    const double period = ps2::g_options.pal ? 20.0 : 1000.0 / 59.94;
    const double now = emscripten_get_now();
    if (g_next_frame == 0.0 || now - g_next_frame > 200.0) g_next_frame = now;
    g_next_frame += period;
    if (g_next_frame > now) usleep(static_cast<useconds_t>((g_next_frame - now) * 1000.0));
}

void on_vblank() {
    const double start = emscripten_get_now();
    pace();
    g_emulation_idle.store(g_emulation_idle.load() + emscripten_get_now() - start);
}

struct Launch {
    std::vector<std::string> args;
};

void *emulate(void *data) {
    const Launch &launch = *static_cast<const Launch *>(data);
    if (g_disc_size && !ps2::disc::open("disc", g_disc_size, read_disc)) std::fprintf(stderr, "[web] cannot read the disc image\n");
    std::uint32_t entry = 0;
    if (!ps2::load_elf("/game/core.elf", entry)) {
        std::fprintf(stderr, "cannot load the game executable\n");
        return nullptr;
    }
    ps2::boot(entry, launch.args);
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
extern "C" EMSCRIPTEN_KEEPALIVE double ps2web_idle_ms(int which) {
    if (which == 2) return ps2::vu_thread_idle_ms();
    return which ? g_render_idle.load() : g_emulation_idle.load();
}

int main(int argc, char **argv) {
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
    g_disc_size = static_cast<std::uint64_t>(ps2web_disc_size());
    g_disc_url = ps2web_disc_url();
    ps2::g_options.on_vblank = on_vblank;
    ps2::g_options.on_vu_program = [](const std::string &name, const std::uint8_t *code, std::size_t size) {
        static bool unsupported = false;
        if (unsupported) return;
        const int status = ps2web_save_program(name.c_str(), code, static_cast<int>(size));
        if (status == 200 || status == 201) std::fprintf(stderr, "[web] saved VU1 program %s\n", name.c_str());
        else unsupported = true;
    };
    ps2::set_audio_sink(push_audio);
    register_generated_code();
    register_generated_vu();

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
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        ps2::start_vu_thread();
        pthread_t vu_thread;
        if (pthread_create(&vu_thread, &attr, [](void *) -> void * {
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
    for (;;) {
        service_disc();
        if (ps2::render_pending(surface, copy ? blit : none)) {
            ps2web_frame_done();
            ps2::browser_yield();
        } else {
            const double start = emscripten_get_now();
            ps2::wait_render_work(50);
            g_render_idle.store(g_render_idle.load() + emscripten_get_now() - start);
        }
    }
}
