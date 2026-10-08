#include "gpu.hpp"

#include <webgpu/webgpu.h>
#ifndef __EMSCRIPTEN__
#include <webgpu/wgpu.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

extern const char kGsShader[];

namespace ps2::gs {
void swizzle_tables(std::vector<std::uint32_t> &out);
}

namespace ps2::gs::gpu {

namespace {

enum Pipeline { kUpload, kCopyRead, kCopyWrite, kLoadClut, kDisplay, kRaster, kPipelines };
const char *const kEntryPoints[kPipelines] = {"upload", "copy_read", "copy_write", "load_clut", "display", "raster"};

constexpr std::uint32_t kParamStride = 256;
constexpr std::uint32_t kClutSlots = 2048;
constexpr std::uint32_t kClutEntries = 512;
constexpr std::uint64_t kScratchBytes = 16ull << 20;
constexpr std::uint32_t kPrimWords = 40;
constexpr std::uint32_t kSetupWords = 24;
constexpr int kTileShift = 4;
constexpr int kTilesPerRow = 2048 >> kTileShift;

Mode g_mode = Mode::off;
std::uint64_t g_dispatches[kPipelines] = {}, g_submits = 0;

WGPUInstance g_instance = nullptr;
WGPUDevice g_device = nullptr;
WGPUQueue g_queue = nullptr;
WGPUBuffer g_vram = nullptr, g_tables = nullptr, g_params = nullptr, g_data = nullptr, g_clut = nullptr, g_scratch = nullptr, g_readback = nullptr;
std::uint64_t g_params_size = 0, g_data_size = 0;
WGPUBindGroupLayout g_layout = nullptr;
WGPUPipelineLayout g_pipeline_layout = nullptr;
WGPUBindGroup g_group = nullptr;
WGPUComputePipeline g_pipelines[kPipelines] = {};

struct Command {
    Pipeline pipeline;
    std::uint32_t x, y;
    std::uint32_t param;
};

std::vector<Command> g_commands;
std::vector<std::array<std::uint32_t, kParamStride / 4>> g_param_records;
struct Arena {
    std::unique_ptr<std::uint32_t[]> words;
    std::size_t used = 0, capacity = 0;

    std::uint32_t *grow(std::size_t count) {
        if (used + count > capacity) {
            std::size_t next = std::max<std::size_t>(capacity * 2, 1u << 20);
            while (next < used + count) next *= 2;
            auto bigger = std::make_unique_for_overwrite<std::uint32_t[]>(next);
            if (used) std::memcpy(bigger.get(), words.get(), used * 4);
            words = std::move(bigger);
            capacity = next;
        }
        std::uint32_t *at = words.get() + used;
        used += count;
        return at;
    }
    std::size_t size() const { return used; }
    bool empty() const { return !used; }
    void clear() { used = 0; }
    std::uint32_t *data() { return words.get(); }
    std::uint32_t &operator[](std::size_t i) { return words[i]; }
} g_arena;
std::uint32_t g_clut_slot = 0, g_clut_loads = 0;

struct Image {
    bool active = false;
    std::uint32_t dbp = 0, dbw = 0, dpsm = 0, dsax = 0, dsay = 0, rrw = 0, total = 0, done = 0, bpp = 32, lead_bits = 0;
    std::vector<std::uint8_t> pending;
} g_image;

struct Range {
    std::uint64_t lo = ~0ull, hi = 0;
    bool overlaps(std::uint64_t a, std::uint64_t b) const { return lo < b && a < hi; }
    void add(std::uint64_t a, std::uint64_t b) {
        lo = std::min(lo, a);
        hi = std::max(hi, b);
    }
};

struct Batch {
    std::vector<std::uint32_t> prims;
    std::vector<std::uint32_t> setups;
    std::vector<std::uint32_t> boxes;
    std::uint64_t key = ~0ull;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
    std::uint32_t fbp = 0, fbw = 0, fpsm = 0, zbp = 0, zpsm = 0;
    bool zte = false;
} g_batch;

struct PreOp {
    bool clut = false;
    std::uint32_t rec[12] = {};
    Range range;
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

bool uploads_overlap(const PreOp &a, const PreOp &b) {
    if (!a.range.overlaps(b.range.lo, b.range.hi)) return false;
    if (a.rec[0] != b.rec[0] || a.rec[1] != b.rec[1] || a.rec[2] != b.rec[2]) return true;
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

// Uploads and CLUT loads run before the pending primitive batch unless they touch memory it reads or writes.
std::vector<PreOp> g_pre;
std::vector<Range> g_batch_reads;

std::vector<std::uint32_t> g_tile_count(kTilesPerRow * kTilesPerRow), g_tile_fill(kTilesPerRow * kTilesPerRow);
std::vector<std::uint32_t> g_touched;

WGPUStringView view(const char *s) { return WGPUStringView{s, WGPU_STRLEN}; }

std::uint32_t bits_per_pixel(std::uint32_t psm) {
    switch (psm) {
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    case PSMT4: case PSMT4HL: case PSMT4HH: return 4;
    default: return 32;
    }
}

void page_size(std::uint32_t psm, std::uint32_t &pw, std::uint32_t &ph) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pw = 64; ph = 64; break;
    case PSMT8: pw = 128; ph = 64; break;
    case PSMT4: pw = 128; ph = 128; break;
    default: pw = 64; ph = 32; break;
    }
}

Range region(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, int x0, int y0, int x1, int y1) {
    std::uint32_t pw, ph;
    page_size(psm, pw, ph);
    const std::uint64_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    const auto page = [&](int x, int y) {
        return static_cast<std::uint64_t>(bp) * 256 + (static_cast<std::uint64_t>(std::max(x, 0)) / pw + static_cast<std::uint64_t>(std::max(y, 0)) / ph * per_row) * 8192;
    };
    Range r;
    r.add(std::min(page(x0, y0), static_cast<std::uint64_t>(bp) * 256), page(x1, y1) + 8192);
    return r;
}

std::uint32_t z_max(std::uint32_t psm) {
    switch (psm) {
    case PSMZ24: return 0xFFFFFFu;
    case PSMZ16: case PSMZ16S: return 0xFFFFu;
    default: return 0xFFFFFFFFu;
    }
}

std::uint32_t f2u(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

std::uint32_t push_params(const std::initializer_list<std::uint32_t> values) {
    std::array<std::uint32_t, kParamStride / 4> record{};
    std::size_t i = 0;
    for (std::uint32_t v : values) record[i++] = v;
    g_param_records.push_back(record);
    return static_cast<std::uint32_t>(g_param_records.size() - 1);
}

void dispatch_linear(Pipeline p, std::uint32_t items, std::uint32_t param) {
    const std::uint32_t groups = (items + 63) / 64;
    if (!groups) return;
    g_commands.push_back({p, std::min(groups, 1024u), (groups + 1023) / 1024, param});
}

std::uint32_t arena_append(const void *bytes, std::size_t size) {
    const std::uint32_t offset = static_cast<std::uint32_t>(g_arena.size());
    const std::size_t words = (size + 3) / 4 + 1;
    std::uint32_t *to = g_arena.grow(words);
    to[words - 2] = 0;
    to[words - 1] = 0;
    std::memcpy(to, bytes, size);
    return offset;
}

void flush_batch() {
    Batch &b = g_batch;
    if (b.prims.empty()) {
        b.key = ~0ull;
        return;
    }
    const std::uint32_t prims = static_cast<std::uint32_t>(b.prims.size() / kPrimWords);
    g_touched.clear();
    for (std::uint32_t i = 0; i < prims; ++i) {
        const std::uint32_t lo = b.boxes[i * 2], hi = b.boxes[i * 2 + 1];
        const int tx0 = static_cast<int>(lo & 0xFFFFu) >> kTileShift, ty0 = static_cast<int>(lo >> 16) >> kTileShift;
        const int tx1 = static_cast<int>(hi & 0xFFFFu) >> kTileShift, ty1 = static_cast<int>(hi >> 16) >> kTileShift;
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                const std::uint32_t t = static_cast<std::uint32_t>(ty * kTilesPerRow + tx);
                if (!g_tile_count[t]++) g_touched.push_back(t);
            }
    }
    const std::uint32_t prim_offset = arena_append(b.prims.data(), b.prims.size() * 4);
    const std::uint32_t setup_offset = arena_append(b.setups.data(), b.setups.size() * 4);
    std::uint32_t total = 0;
    for (std::uint32_t t : g_touched) total += g_tile_count[t];
    const std::uint32_t tiles = static_cast<std::uint32_t>(g_touched.size());
    const std::uint32_t tile_offset = static_cast<std::uint32_t>(g_arena.size());
    const std::uint32_t list_offset = tile_offset + tiles * 4;
    g_arena.grow(tiles * 4 + total);
    std::uint32_t cursor = list_offset;
    for (std::uint32_t i = 0; i < tiles; ++i) {
        const std::uint32_t t = g_touched[i];
        std::uint32_t *rec = g_arena.data() + tile_offset + i * 4;
        rec[3] = 0;
        rec[0] = (t % kTilesPerRow) | ((t / kTilesPerRow) << 16);
        rec[1] = cursor;
        rec[2] = g_tile_count[t];
        g_tile_fill[t] = cursor;
        cursor += g_tile_count[t];
    }
    for (std::uint32_t i = 0; i < prims; ++i) {
        const std::uint32_t lo = b.boxes[i * 2], hi = b.boxes[i * 2 + 1];
        const int tx0 = static_cast<int>(lo & 0xFFFFu) >> kTileShift, ty0 = static_cast<int>(lo >> 16) >> kTileShift;
        const int tx1 = static_cast<int>(hi & 0xFFFFu) >> kTileShift, ty1 = static_cast<int>(hi >> 16) >> kTileShift;
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) g_arena[g_tile_fill[static_cast<std::uint32_t>(ty * kTilesPerRow + tx)]++] = i;
    }
    for (std::uint32_t t : g_touched) g_tile_count[t] = 0;
    const std::uint32_t param = push_params({tile_offset, prim_offset, setup_offset, tiles});
    g_commands.push_back({kRaster, std::min(tiles, 1024u), (tiles + 1023) / 1024, param});
    b.prims.clear();
    b.setups.clear();
    b.boxes.clear();
    g_batch_reads.clear();
    b.key = ~0ull;
    b.minx = b.miny = 1 << 30;
    b.maxx = b.maxy = -1;
}

bool batch_conflict(const Range &r, bool clut) {
    const Batch &b = g_batch;
    if (b.prims.empty()) return false;
    const Range fb = region(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);
    if (fb.overlaps(r.lo, r.hi)) return true;
    if (b.zte) {
        const Range z = region(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy);
        if (z.overlaps(r.lo, r.hi)) return true;
    }
    if (!clut)
        for (const Range &t : g_batch_reads)
            if (t.overlaps(r.lo, r.hi)) return true;
    return false;
}

void emit_pre() {
    std::size_t i = 0;
    while (i < g_pre.size()) {
        const bool clut = g_pre[i].clut;
        std::size_t j = i + 1;
        while (j < g_pre.size() && g_pre[j].clut == clut) {
            bool overlap = false;
            if (!clut)
                for (std::size_t k = i; k < j && !overlap; ++k) overlap = uploads_overlap(g_pre[k], g_pre[j]);
            if (overlap) break;
            ++j;
        }
        const std::uint32_t count = static_cast<std::uint32_t>(j - i);
        std::uint32_t *records = g_arena.grow(count * 12u);
        const std::uint32_t offset = static_cast<std::uint32_t>(records - g_arena.data());
        std::uint32_t total = 0;
        for (std::size_t k = i; k < j; ++k) {
            std::memcpy(records + (k - i) * 12, g_pre[k].rec, sizeof g_pre[k].rec);
            if (!clut) {
                records[(k - i) * 12 + 11] = total;
                total += g_pre[k].rec[7];
            }
        }
        if (clut) g_commands.push_back({kLoadClut, 1, 1, push_params({offset, count})});
        else dispatch_linear(kUpload, total, push_params({offset, count, total}));
        i = j;
    }
    g_pre.clear();
}

void emit_all() {
    emit_pre();
    flush_batch();
}

void queue_pre(const PreOp &op) {
    if (batch_conflict(op.range, op.clut)) emit_all();
    g_pre.push_back(op);
}

void flush_image() {
    Image &im = g_image;
    if (!im.active || im.pending.empty()) return;
    const std::uint64_t bits = static_cast<std::uint64_t>(im.pending.size()) * 8;
    if (bits <= im.lead_bits) return;
    std::uint32_t pixels = static_cast<std::uint32_t>((bits - im.lead_bits) / im.bpp);
    pixels = std::min(pixels, im.total - im.done);
    if (!pixels) return;
    PreOp op;
    const std::uint32_t offset = arena_append(im.pending.data(), im.pending.size());
    const std::uint32_t rec[11] = {im.dbp, im.dbw, im.dpsm, im.dsax, im.dsay, im.rrw, im.done, pixels, offset, im.lead_bits, im.bpp};
    std::memcpy(op.rec, rec, sizeof rec);
    const std::uint32_t first = im.done / im.rrw, last = (im.done + pixels - 1) / im.rrw;
    op.range = region(im.dbp, im.dbw, im.dpsm, static_cast<int>(im.dsax), static_cast<int>(im.dsay + first), static_cast<int>(im.dsax + im.rrw - 1),
                      static_cast<int>(im.dsay + last));
    op.x0 = im.dsax;
    op.x1 = im.dsax + im.rrw - 1;
    op.y0 = im.dsay + (im.done % im.rrw ? first : first);
    op.y1 = im.dsay + last;
    queue_pre(op);
    const std::uint64_t used = im.lead_bits + static_cast<std::uint64_t>(pixels) * im.bpp;
    im.pending.erase(im.pending.begin(), im.pending.begin() + static_cast<std::ptrdiff_t>(used / 8));
    im.lead_bits = static_cast<std::uint32_t>(used % 8);
    im.done += pixels;
    if (im.done >= im.total) {
        im.active = false;
        im.pending.clear();
    }
}

void flush_all() {
    flush_image();
    emit_all();
}

void on_error(WGPUDevice const *, WGPUErrorType type, WGPUStringView message, void *, void *) {
    std::fprintf(stderr, "[gpu] error %d: %.*s\n", static_cast<int>(type), static_cast<int>(message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length), message.data);
}

void wait_until(const bool &flag) {
    while (!flag) {
#ifdef __EMSCRIPTEN__
        wgpuInstanceProcessEvents(g_instance);
#else
        wgpuDevicePoll(g_device, true, nullptr);
#endif
    }
}

WGPUBuffer make_buffer(std::uint64_t size, WGPUBufferUsage usage) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = size;
    d.usage = usage;
    return wgpuDeviceCreateBuffer(g_device, &d);
}

void rebuild_group() {
    if (g_group) wgpuBindGroupRelease(g_group);
    WGPUBindGroupEntry e[6] = {};
    WGPUBuffer buffers[6] = {g_vram, g_tables, g_params, g_data, g_clut, g_scratch};
    for (int i = 0; i < 6; ++i) {
        e[i] = WGPU_BIND_GROUP_ENTRY_INIT;
        e[i].binding = static_cast<std::uint32_t>(i);
        e[i].buffer = buffers[i];
        e[i].size = i == 2 ? kParamStride : WGPU_WHOLE_SIZE;
    }
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = g_layout;
    d.entryCount = 6;
    d.entries = e;
    g_group = wgpuDeviceCreateBindGroup(g_device, &d);
}

void ensure_capacity() {
    bool changed = false;
    const std::uint64_t data_bytes = std::max<std::uint64_t>(g_arena.size() * 4, 16);
    if (data_bytes > g_data_size) {
        if (g_data) wgpuBufferRelease(g_data);
        g_data_size = std::max<std::uint64_t>(g_data_size * 2, (data_bytes + (4u << 20) - 1) & ~((4ull << 20) - 1));
        g_data = make_buffer(g_data_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        changed = true;
    }
    const std::uint64_t param_bytes = std::max<std::uint64_t>(g_param_records.size(), 1) * kParamStride;
    if (param_bytes > g_params_size) {
        if (g_params) wgpuBufferRelease(g_params);
        g_params_size = std::max<std::uint64_t>(g_params_size * 2, std::max<std::uint64_t>(param_bytes, 64 * 1024));
        g_params = make_buffer(g_params_size, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
        changed = true;
    }
    if (changed || !g_group) rebuild_group();
}

void submit() {
    if (g_commands.empty()) return;
    ensure_capacity();
    if (!g_arena.empty()) wgpuQueueWriteBuffer(g_queue, g_data, 0, g_arena.data(), g_arena.size() * 4);
    wgpuQueueWriteBuffer(g_queue, g_params, 0, g_param_records.data(), g_param_records.size() * kParamStride);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    Pipeline current = kPipelines;
    ++g_submits;
    for (const Command &c : g_commands) {
        ++g_dispatches[c.pipeline];
        if (c.pipeline != current) {
            wgpuComputePassEncoderSetPipeline(pass, g_pipelines[c.pipeline]);
            current = c.pipeline;
        }
        const std::uint32_t offset = c.param * kParamStride;
        wgpuComputePassEncoderSetBindGroup(pass, 0, g_group, 1, &offset);
        wgpuComputePassEncoderDispatchWorkgroups(pass, c.x, c.y, 1);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    g_commands.clear();
    g_param_records.clear();
    g_arena.clear();
    g_clut_loads = 0;
}

void read_back(WGPUBuffer source, std::uint64_t bytes, void *out) {
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, source, 0, g_readback, 0, bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    bool done = false;
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *flag, void *) {
        if (status != WGPUMapAsyncStatus_Success) std::fprintf(stderr, "[gpu] map failed %d\n", static_cast<int>(status));
        *static_cast<bool *>(flag) = true;
    };
    cb.userdata1 = &done;
    wgpuBufferMapAsync(g_readback, WGPUMapMode_Read, 0, bytes, cb);
    wait_until(done);
    const void *mapped = wgpuBufferGetConstMappedRange(g_readback, 0, bytes);
    if (mapped) std::memcpy(out, mapped, bytes);
    wgpuBufferUnmap(g_readback);
}

std::uint32_t make_setup(const State &st, std::uint64_t attr, std::uint32_t *w) {
    const Context &c = st.ctx[(attr >> 9) & 1u];
    std::uint32_t flags = 0;
    if (attr & 0x8u) flags |= 1u;
    if (attr & 0x10u) flags |= 2u;
    if (attr & 0x20u) flags |= 4u;
    if (attr & 0x40u) flags |= 8u;
    if (attr & 0x100u) flags |= 16u;
    if ((c.zbuf >> 32) & 1u) flags |= 32u;
    if (c.test & 1u) flags |= 64u;
    if ((c.test >> 14) & 1u) flags |= 128u;
    if ((c.test >> 15) & 1u) flags |= 256u;
    if ((c.test >> 16) & 1u) flags |= 512u;
    if (st.pabe & 1u) flags |= 1024u;
    if (st.colclamp & 1u) flags |= 2048u;
    if (c.fba & 1u) flags |= 4096u;
    if ((c.tex0 >> 34) & 1u) flags |= 8192u;
    if ((c.tex1 >> 5) & 1u) flags |= 16384u;
    if ((st.texa >> 15) & 1u) flags |= 32768u;
    const std::uint32_t zpsm = 0x30u | (static_cast<std::uint32_t>(c.zbuf >> 24) & 0xFu);
    w[0] = flags;
    w[1] = (static_cast<std::uint32_t>(c.frame) & 0x1FFu) * 32u;
    w[2] = static_cast<std::uint32_t>(c.frame >> 16) & 0x3Fu;
    w[3] = static_cast<std::uint32_t>(c.frame >> 24) & 0x3Fu;
    w[4] = static_cast<std::uint32_t>(c.frame >> 32);
    w[5] = (static_cast<std::uint32_t>(c.zbuf) & 0x1FFu) * 32u;
    w[6] = zpsm;
    w[7] = (static_cast<std::uint32_t>(c.test >> 1) & 7u) | ((static_cast<std::uint32_t>(c.test >> 12) & 3u) << 4) |
           ((static_cast<std::uint32_t>(c.test >> 17) & 3u) << 8) | ((static_cast<std::uint32_t>(c.alpha) & 0xFFu) << 12) |
           ((static_cast<std::uint32_t>(c.tex0 >> 35) & 3u) << 20) | ((static_cast<std::uint32_t>(c.clamp) & 0xFu) << 24) |
           ((static_cast<std::uint32_t>(st.scanmsk) & 3u) << 28);
    w[8] = static_cast<std::uint32_t>(c.test >> 4) & 0xFFu;
    w[9] = static_cast<std::uint32_t>(c.alpha >> 32) & 0xFFu;
    w[10] = (static_cast<std::uint32_t>(c.scissor) & 0x7FFu) | ((static_cast<std::uint32_t>(c.scissor >> 16) & 0x7FFu) << 16);
    w[11] = (static_cast<std::uint32_t>(c.scissor >> 32) & 0x7FFu) | ((static_cast<std::uint32_t>(c.scissor >> 48) & 0x7FFu) << 16);
    w[12] = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
    w[13] = static_cast<std::uint32_t>(c.tex0 >> 14) & 0x3Fu;
    w[14] = static_cast<std::uint32_t>(c.tex0 >> 20) & 0x3Fu;
    w[15] = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 26) & 0xFu, 10u) | (std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 30) & 0xFu, 10u) << 8);
    w[16] = static_cast<std::uint32_t>(c.tex0 >> 51) & 0xFu;
    w[17] = static_cast<std::uint32_t>(c.tex0 >> 56) & 0x1Fu;
    w[18] = (static_cast<std::uint32_t>(c.clamp >> 4) & 0x3FFu) | ((static_cast<std::uint32_t>(c.clamp >> 14) & 0x3FFu) << 16);
    w[19] = (static_cast<std::uint32_t>(c.clamp >> 24) & 0x3FFu) | ((static_cast<std::uint32_t>(c.clamp >> 34) & 0x3FFu) << 16);
    w[20] = (static_cast<std::uint32_t>(st.texa) & 0xFFu) | ((static_cast<std::uint32_t>(st.texa >> 32) & 0xFFu) << 8);
    w[21] = static_cast<std::uint32_t>(st.fogcol) & 0xFFFFFFu;
    w[22] = g_clut_slot;
    w[23] = z_max(zpsm);
    return flags;
}

void attrs(std::uint32_t *slot, const Vertex &v, bool fst, const Vertex *color) {
    const Vertex &c = color ? *color : v;
    const float a[8] = {static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b), static_cast<float>(c.a),
                        fst ? static_cast<float>(v.u) : v.s, fst ? static_cast<float>(v.v) : v.t, fst ? 1.0f : v.q, static_cast<float>(v.f)};
    for (int i = 0; i < 8; ++i) slot[i] = f2u(a[i]);
}

}

Mode mode() { return g_mode; }

bool init(Mode m) {
    if (m == Mode::off) return false;
    g_instance = wgpuCreateInstance(nullptr);
    if (!g_instance) return false;
    WGPUAdapter adapter = nullptr;
    bool adapter_done = false;
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.powerPreference = std::getenv("PS2_GPU_LOW_POWER") ? WGPUPowerPreference_LowPower : WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    acb.mode = WGPUCallbackMode_AllowSpontaneous;
    acb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView, void *out, void *done) {
        if (status == WGPURequestAdapterStatus_Success) *static_cast<WGPUAdapter *>(out) = a;
        *static_cast<bool *>(done) = true;
    };
    acb.userdata1 = &adapter;
    acb.userdata2 = &adapter_done;
    wgpuInstanceRequestAdapter(g_instance, &options, acb);
    while (!adapter_done) wgpuInstanceProcessEvents(g_instance);
    if (!adapter) {
        std::fprintf(stderr, "[gpu] no adapter\n");
        return false;
    }
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
        std::fprintf(stderr, "[gpu] %.*s (%.*s)\n", static_cast<int>(info.device.length), info.device.data, static_cast<int>(info.description.length), info.description.data);
        wgpuAdapterInfoFreeMembers(info);
    }
    WGPULimits supported = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(adapter, &supported);
    WGPULimits limits = WGPU_LIMITS_INIT;
    limits.maxStorageBufferBindingSize = supported.maxStorageBufferBindingSize;
    limits.maxBufferSize = supported.maxBufferSize;
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.requiredLimits = &limits;
    dd.uncapturedErrorCallbackInfo.callback = on_error;
    bool device_done = false;
    WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dcb.mode = WGPUCallbackMode_AllowSpontaneous;
    dcb.callback = [](WGPURequestDeviceStatus status, WGPUDevice d, WGPUStringView message, void *out, void *done) {
        if (status == WGPURequestDeviceStatus_Success) *static_cast<WGPUDevice *>(out) = d;
        else std::fprintf(stderr, "[gpu] device: %.*s\n", static_cast<int>(message.length), message.data);
        *static_cast<bool *>(done) = true;
    };
    dcb.userdata1 = &g_device;
    dcb.userdata2 = &device_done;
    wgpuAdapterRequestDevice(adapter, &dd, dcb);
    while (!device_done) wgpuInstanceProcessEvents(g_instance);
    wgpuAdapterRelease(adapter);
    if (!g_device) return false;
    g_queue = wgpuDeviceGetQueue(g_device);

    g_vram = make_buffer(kMemorySize, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
    std::vector<std::uint32_t> tables;
    swizzle_tables(tables);
    g_tables = make_buffer(tables.size() * 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    wgpuQueueWriteBuffer(g_queue, g_tables, 0, tables.data(), tables.size() * 4);
    g_clut = make_buffer(static_cast<std::uint64_t>(kClutSlots) * kClutEntries * 4, WGPUBufferUsage_Storage);
    g_scratch = make_buffer(kScratchBytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
    g_readback = make_buffer(kScratchBytes, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);

    WGPUBindGroupLayoutEntry le[6] = {};
    const WGPUBufferBindingType types[6] = {WGPUBufferBindingType_Storage, WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Uniform,
                                            WGPUBufferBindingType_ReadOnlyStorage, WGPUBufferBindingType_Storage, WGPUBufferBindingType_Storage};
    for (int i = 0; i < 6; ++i) {
        le[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        le[i].binding = static_cast<std::uint32_t>(i);
        le[i].visibility = WGPUShaderStage_Compute;
        le[i].buffer.type = types[i];
        le[i].buffer.hasDynamicOffset = i == 2;
        le[i].buffer.minBindingSize = i == 2 ? kParamStride : 0;
    }
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 6;
    ld.entries = le;
    g_layout = wgpuDeviceCreateBindGroupLayout(g_device, &ld);
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &g_layout;
    g_pipeline_layout = wgpuDeviceCreatePipelineLayout(g_device, &pld);

    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = view(kGsShader);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device, &md);
    for (int i = 0; i < kPipelines; ++i) {
        WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cd.layout = g_pipeline_layout;
        cd.compute.module = module;
        cd.compute.entryPoint = view(kEntryPoints[i]);
        g_pipelines[i] = wgpuDeviceCreateComputePipeline(g_device, &cd);
        if (!g_pipelines[i]) return false;
    }
    wgpuShaderModuleRelease(module);
    ensure_capacity();
    g_mode = m;
    return true;
}

void draw(const State &st, const Vertex *v, int count) {
    if (g_image.active && !g_image.pending.empty()) flush_image();
    std::uint64_t attr = st.prim;
    if (!(st.prmodecont & 1u)) attr = (st.prim & 7u) | (st.prmode & ~7ull);
    const std::uint32_t prim = static_cast<std::uint32_t>(st.prim) & 7u;
    const Context &c = st.ctx[(attr >> 9) & 1u];
    std::uint32_t setup[kSetupWords];
    const std::uint32_t flags = make_setup(st, attr, setup);
    const bool fst = flags & 16u, iip = flags & 1u;
    const int ofx = static_cast<int>(c.xyoffset & 0xFFFFu), ofy = static_cast<int>((c.xyoffset >> 32) & 0xFFFFu);
    const int scax0 = static_cast<int>(setup[10] & 0xFFFFu), scax1 = static_cast<int>(setup[10] >> 16);
    const int scay0 = static_cast<int>(setup[11] & 0xFFFFu), scay1 = static_cast<int>(setup[11] >> 16);
    const auto sx = [&](const Vertex &p) { return (static_cast<float>(p.x) - static_cast<float>(ofx)) / 16.0f; };
    const auto sy = [&](const Vertex &p) { return (static_cast<float>(p.y) - static_cast<float>(ofy)) / 16.0f; };

    std::uint32_t rec[kPrimWords] = {};
    int minx, miny, maxx, maxy;
    switch (prim) {
    case 3: case 4: case 5: {
        if (count < 3) return;
        const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]), x2 = sx(v[2]), y2 = sy(v[2]);
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (area == 0.0f) return;
        minx = std::max(scax0, static_cast<int>(std::ceil(std::min({x0, x1, x2}))));
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))) - 1);
        miny = std::max(scay0, static_cast<int>(std::ceil(std::min({y0, y1, y2}))));
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))) - 1);
        rec[1] = 0;
        const float xy[6] = {x0, y0, x1, y1, x2, y2};
        for (int i = 0; i < 6; ++i) rec[4 + i] = f2u(xy[i]);
        rec[10] = f2u(1.0f / area);
        for (int i = 0; i < 3; ++i) attrs(rec + 12 + i * 8, v[i], fst, iip ? nullptr : &v[2]);
        rec[36] = v[0].z;
        rec[37] = f2u(static_cast<float>(static_cast<std::int64_t>(v[1].z) - static_cast<std::int64_t>(v[0].z)));
        rec[38] = f2u(static_cast<float>(static_cast<std::int64_t>(v[2].z) - static_cast<std::int64_t>(v[0].z)));
        break;
    }
    case 6: {
        const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]);
        minx = std::max(scax0, static_cast<int>(std::ceil(std::min(x0, x1))));
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max(x0, x1))) - 1);
        miny = std::max(scay0, static_cast<int>(std::ceil(std::min(y0, y1))));
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max(y0, y1))) - 1);
        rec[1] = 1;
        rec[4] = f2u(x0);
        rec[5] = f2u(y0);
        rec[6] = f2u(x1 - x0);
        rec[7] = f2u(y1 - y0);
        attrs(rec + 12, v[0], fst, nullptr);
        attrs(rec + 20, v[1], fst, nullptr);
        rec[36] = v[1].z;
        break;
    }
    case 1: case 2: {
        const float x0 = sx(v[0]), y0 = sy(v[0]), x1 = sx(v[1]), y1 = sy(v[1]);
        const int steps = static_cast<int>(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)));
        minx = std::max(scax0, static_cast<int>(std::floor(std::min(x0, x1))) - 1);
        maxx = std::min(scax1, static_cast<int>(std::ceil(std::max(x0, x1))) + 1);
        miny = std::max(scay0, static_cast<int>(std::floor(std::min(y0, y1))) - 1);
        maxy = std::min(scay1, static_cast<int>(std::ceil(std::max(y0, y1))) + 1);
        rec[1] = 2;
        rec[4] = f2u(x0);
        rec[5] = f2u(y0);
        rec[6] = f2u(x1);
        rec[7] = f2u(y1);
        rec[8] = static_cast<std::uint32_t>(steps);
        attrs(rec + 12, v[0], fst, iip ? nullptr : &v[1]);
        attrs(rec + 20, v[1], fst, nullptr);
        rec[36] = v[0].z;
        rec[37] = f2u(static_cast<float>(static_cast<std::int64_t>(v[1].z) - static_cast<std::int64_t>(v[0].z)));
        break;
    }
    case 0: {
        const int x = static_cast<int>(sx(v[0])), y = static_cast<int>(sy(v[0]));
        minx = maxx = x;
        miny = maxy = y;
        if (x < scax0 || x > scax1 || y < scay0 || y > scay1) return;
        rec[1] = 3;
        rec[4] = static_cast<std::uint32_t>(x);
        rec[5] = static_cast<std::uint32_t>(y);
        attrs(rec + 12, v[0], fst, nullptr);
        rec[36] = v[0].z;
        break;
    }
    default:
        return;
    }
    minx = std::max(minx, 0);
    miny = std::max(miny, 0);
    maxx = std::min(maxx, 2047);
    maxy = std::min(maxy, 2047);
    if (minx > maxx || miny > maxy) return;

    Batch &b = g_batch;
    const bool zte = flags & 512u;
    const std::uint64_t key = static_cast<std::uint64_t>(setup[1]) | (static_cast<std::uint64_t>(setup[2]) << 14) | (static_cast<std::uint64_t>(setup[3]) << 20) |
                              (static_cast<std::uint64_t>(setup[5]) << 26) | (static_cast<std::uint64_t>(setup[6]) << 40);
    if (b.key != ~0ull && b.key != key) emit_all();
    Range tex;
    if (flags & 2u) {
        const std::uint32_t tw = setup[15] & 0xFFu, th = setup[15] >> 8;
        const int w = std::max(1 << tw, static_cast<int>(setup[18] >> 16) + 1), h = std::max(1 << th, static_cast<int>(setup[19] >> 16) + 1);
        tex = region(setup[12], setup[13], setup[14], 0, 0, w - 1, h - 1);
        if (!b.prims.empty()) {
            const Range fb = region(b.fbp, b.fbw, b.fpsm, b.minx, b.miny, b.maxx, b.maxy);
            bool hazard = tex.overlaps(fb.lo, fb.hi);
            if (!hazard && b.zte) {
                const Range z = region(b.zbp, b.fbw, b.zpsm, b.minx, b.miny, b.maxx, b.maxy);
                hazard = tex.overlaps(z.lo, z.hi);
            }
            if (hazard) emit_all();
        }
    }
    if (b.prims.empty()) {
        b.key = key;
        b.fbp = setup[1];
        b.fbw = setup[2];
        b.fpsm = setup[3];
        b.zbp = setup[5];
        b.zpsm = setup[6];
        b.zte = false;
    }
    b.zte = b.zte || zte;
    const std::size_t setups = b.setups.size() / kSetupWords;
    std::uint32_t setup_index;
    if (setups && std::memcmp(b.setups.data() + (setups - 1) * kSetupWords, setup, sizeof setup) == 0) {
        setup_index = static_cast<std::uint32_t>(setups - 1);
    } else {
        setup_index = static_cast<std::uint32_t>(setups);
        b.setups.insert(b.setups.end(), setup, setup + kSetupWords);
    }
    if (flags & 2u) {
        if (g_batch_reads.empty() || g_batch_reads.back().lo != tex.lo || g_batch_reads.back().hi != tex.hi) {
            bool known = false;
            for (const Range &t : g_batch_reads) known = known || (t.lo <= tex.lo && tex.hi <= t.hi);
            if (!known) g_batch_reads.push_back(tex);
        }
    }
    rec[0] = setup_index;
    rec[2] = static_cast<std::uint32_t>(minx) | (static_cast<std::uint32_t>(miny) << 16);
    rec[3] = static_cast<std::uint32_t>(maxx) | (static_cast<std::uint32_t>(maxy) << 16);
    b.prims.insert(b.prims.end(), rec, rec + kPrimWords);
    b.boxes.push_back(rec[2]);
    b.boxes.push_back(rec[3]);
    b.minx = std::min(b.minx, minx);
    b.miny = std::min(b.miny, miny);
    b.maxx = std::max(b.maxx, maxx);
    b.maxy = std::max(b.maxy, maxy);
    if (b.prims.size() >= kPrimWords * 65536) emit_all();
}

void image_begin(const State &s) {
    flush_image();
    Image &im = g_image;
    const std::uint64_t buf = s.bitbltbuf, pos = s.trxpos, reg = s.trxreg;
    im = Image{};
    im.dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu;
    im.dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
    im.dpsm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    im.dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu;
    im.dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    im.rrw = static_cast<std::uint32_t>(reg) & 0xFFFu;
    im.total = im.rrw * (static_cast<std::uint32_t>(reg >> 32) & 0xFFFu);
    im.bpp = bits_per_pixel(im.dpsm);
    im.active = im.total != 0;
}

void image_data(const std::uint8_t *data, std::size_t bytes) {
    Image &im = g_image;
    if (!im.active) return;
    im.pending.insert(im.pending.end(), data, data + bytes);
}

void local_copy(const State &s) {
    flush_all();
    const std::uint64_t buf = s.bitbltbuf, pos = s.trxpos, reg = s.trxreg;
    const std::uint32_t w = static_cast<std::uint32_t>(reg) & 0xFFFu, h = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (!w || !h) return;
    const std::uint32_t read = push_params({static_cast<std::uint32_t>(buf) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, static_cast<std::uint32_t>(buf >> 24) & 0x3Fu,
                                            static_cast<std::uint32_t>(pos) & 0x7FFu, static_cast<std::uint32_t>(pos >> 16) & 0x7FFu, w, h});
    const std::uint32_t write = push_params({static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, static_cast<std::uint32_t>(buf >> 56) & 0x3Fu,
                                             static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, static_cast<std::uint32_t>(pos >> 48) & 0x7FFu, w, h,
                                             static_cast<std::uint32_t>(pos >> 59) & 3u});
    dispatch_linear(kCopyRead, w * h, read);
    dispatch_linear(kCopyWrite, w * h, write);
}

void load_clut(std::uint64_t tex0, std::uint64_t texclut) {
    flush_image();
    if (g_clut_loads + 2 >= kClutSlots) {
        emit_all();
        submit();
    }
    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3Fu;
    const std::uint32_t cpsm = (static_cast<std::uint32_t>(tex0 >> 51) & 0xFu) == 0 ? PSMCT32 : static_cast<std::uint32_t>(tex0 >> 51) & 0xFu;
    const std::uint32_t next = (g_clut_slot + 1) % kClutSlots;
    const std::uint32_t tc = static_cast<std::uint32_t>(texclut);
    const std::uint32_t cbp = static_cast<std::uint32_t>(tex0 >> 37) & 0x3FFFu, csm = static_cast<std::uint32_t>(tex0 >> 55) & 1u;
    const std::uint32_t cbw = tc & 0x3Fu, cou = (tc >> 6) & 0x3Fu, cov = (tc >> 12) & 0x3FFu;
    PreOp op;
    op.clut = true;
    const std::uint32_t rec[10] = {g_clut_slot, next, cbp, cpsm, csm, static_cast<std::uint32_t>(tex0 >> 56) & 0x1Fu, (psm == PSMT8 || psm == PSMT8H) ? 1u : 0u, cbw, cou, cov};
    std::memcpy(op.rec, rec, sizeof rec);
    op.range = csm ? region(cbp, cbw, cpsm, static_cast<int>(cou * 16), static_cast<int>(cov), static_cast<int>(cou * 16 + 255), static_cast<int>(cov))
                   : region(cbp, 1, cpsm, 0, 0, 15, 15);
    queue_pre(op);
    g_clut_slot = next;
    ++g_clut_loads;
}

void flush() {
    if (g_mode == Mode::off) return;
    flush_all();
    submit();
}

void download(std::uint8_t *memory) {
    flush_all();
    submit();
    read_back(g_vram, kMemorySize, memory);
}

void stats() {
    if (g_mode == Mode::off) return;
    std::fprintf(stderr, "[gpu] %llu submits, dispatches:", static_cast<unsigned long long>(g_submits));
    for (int i = 0; i < kPipelines; ++i) std::fprintf(stderr, " %s %llu", kEntryPoints[i], static_cast<unsigned long long>(g_dispatches[i]));
    std::fprintf(stderr, "\n");
}

bool display(const DisplaySource &src, std::vector<std::uint32_t> &rgba) {
    flush_all();
    const std::uint32_t param = push_params({src.fbp, src.fbw, src.psm, src.dbx, src.dby, src.width, src.height, src.halved ? 1u : 0u});
    g_commands.push_back({kDisplay, (src.width + 7) / 8, (src.height + 7) / 8, param});
    submit();
    rgba.resize(static_cast<std::size_t>(src.width) * src.height);
    read_back(g_scratch, rgba.size() * 4, rgba.data());
    return true;
}

}
