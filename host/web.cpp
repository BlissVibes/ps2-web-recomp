#include "ps2/disc.hpp"
#include "ps2/system.hpp"

#include <emscripten.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

void register_generated_code();
void register_generated_vu();

EM_ASYNC_JS(int, ps2web_fetch, (double offset, int size, void *out), {
    for (let attempt = 0; attempt < 4; ++attempt) {
        try {
            const response = await fetch(Module.discUrl, {headers: {Range: 'bytes=' + offset + '-' + (offset + size - 1)}});
            if (response.status !== 206 && response.status !== 200) continue;
            const bytes = new Uint8Array(await response.arrayBuffer());
            const length = Math.min(bytes.length, size);
            HEAPU8.set(bytes.subarray(0, length), out);
            return length;
        } catch (e) {
            console.warn('disc fetch failed', e);
        }
    }
    return 0;
});

EM_JS(double, ps2web_disc_size, (), { return Module.discSize || 0; });
EM_JS(void, ps2web_audio, (const std::int16_t *samples, int frames), { Module.pushAudio(HEAP16.subarray(samples >> 1, (samples >> 1) + frames * 2)); });
EM_JS(double, ps2web_audio_queued_ms, (), { return Module.audioQueuedMs ? Module.audioQueuedMs() : 0; });
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

EM_JS(void, ps2web_snapshot, (const std::uint32_t *rgba, int width, int height), {
    const pixels = new Uint8ClampedArray(HEAPU8.buffer, rgba, width * height * 4).slice();
    Module.snapshot = {width: width, height: height, pixels: pixels};
    Module.wantSnapshot = false;
});

namespace {

constexpr std::size_t kChunk = 1u << 20;
constexpr std::size_t kCacheChunks = 64;

std::unordered_map<std::uint64_t, std::list<std::pair<std::uint64_t, std::vector<std::uint8_t>>>::iterator> g_chunks;
std::list<std::pair<std::uint64_t, std::vector<std::uint8_t>>> g_lru;
std::uint64_t g_disc_size = 0;

const std::vector<std::uint8_t> *chunk(std::uint64_t index) {
    auto it = g_chunks.find(index);
    if (it != g_chunks.end()) {
        g_lru.splice(g_lru.begin(), g_lru, it->second);
        return &it->second->second;
    }
    const std::uint64_t offset = index * kChunk;
    const std::size_t size = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, g_disc_size - offset));
    std::vector<std::uint8_t> data(size);
    if (ps2web_fetch(static_cast<double>(offset), static_cast<int>(size), data.data()) <= 0) return nullptr;
    if (g_lru.size() >= kCacheChunks) {
        g_chunks.erase(g_lru.back().first);
        g_lru.pop_back();
    }
    g_lru.emplace_front(index, std::move(data));
    g_chunks[index] = g_lru.begin();
    return &g_lru.front().second;
}

std::size_t read_disc(std::uint64_t offset, void *out, std::size_t size) {
    std::size_t done = 0;
    auto *to = static_cast<std::uint8_t *>(out);
    while (done < size && offset + done < g_disc_size) {
        const std::uint64_t at = offset + done;
        const std::vector<std::uint8_t> *c = chunk(at / kChunk);
        if (!c) break;
        const std::size_t within = static_cast<std::size_t>(at % kChunk);
        const std::size_t take = std::min(size - done, c->size() - within);
        std::memcpy(to + done, c->data() + within, take);
        done += take;
    }
    return done;
}

bool g_window_tried = false, g_window = false, g_copy = false;
std::vector<std::uint32_t> g_frame;

void on_vblank() {
    if (!g_window_tried) {
        g_window_tried = true;
        const bool no_surface = EM_ASM_INT({ return Module.noSurface ? 1 : 0; });
        g_copy = EM_ASM_INT({ return Module.presentCopy ? 1 : 0; });
        if (!g_copy && !no_surface) {
            ps2::WindowTarget target;
            target.kind = ps2::WindowTarget::Canvas;
            target.selector = "#canvas";
            target.width = static_cast<std::uint32_t>(EM_ASM_INT({ return Module.canvas.width; }));
            target.height = static_cast<std::uint32_t>(EM_ASM_INT({ return Module.canvas.height; }));
            g_window = ps2::attach_window(target);
        }
        g_copy = g_copy || (!g_window && !no_surface);
        std::fprintf(stderr, "[web] %s\n", g_window ? "presenting through WebGPU" : g_copy ? "presenting through a 2D canvas copy" : "not presenting");
    }
    if (g_window) ps2::present_frame();
    std::uint32_t w = 0, h = 0;
    if (g_copy && ps2::copy_frame(g_frame, w, h)) ps2web_blit(g_frame.data(), static_cast<int>(w), static_cast<int>(h));
    if (EM_ASM_INT({ return Module.wantSnapshot ? 1 : 0; })) {
        std::vector<std::uint32_t> rgba;
        std::uint32_t w = 0, h = 0;
        if (ps2::display_frame(rgba, w, h)) {
            ps2web_snapshot(rgba.data(), static_cast<int>(w), static_cast<int>(h));
        }
    }
    ps2web_frame_done();
    ps2::browser_yield();
    while (ps2web_audio_queued_ms() > 80.0) emscripten_sleep(2);
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

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ps2web <elf> [--arg text]...\n");
        return 1;
    }
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (ps2::parse_press(argv[++i], press)) ps2::g_options.presses.push_back(press);
        }
    }
    g_disc_size = static_cast<std::uint64_t>(ps2web_disc_size());
    if (g_disc_size && !ps2::disc::open("disc", g_disc_size, read_disc)) std::fprintf(stderr, "[web] cannot read the disc image\n");
    ps2::g_options.on_vblank = on_vblank;
    ps2::set_audio_sink([](const std::int16_t *samples, std::size_t frames) { ps2web_audio(samples, static_cast<int>(frames)); });
    register_generated_code();
    register_generated_vu();
    std::uint32_t entry = 0;
    if (!ps2::load_elf(argv[1], entry)) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    ps2::boot(entry, args);
    return ps2::run();
}
