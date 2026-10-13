#include "spu2.hpp"

#include "hw.hpp"
#include "iop.hpp"
#include "kernel.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace ps2::iop::spu2 {

namespace {

constexpr std::uint32_t kRamWords = 1u << 20;
constexpr std::uint32_t kRamMask = kRamWords - 1;
constexpr std::uint64_t kCyclesPerSample = 768;
constexpr unsigned kBatch = 32;
constexpr unsigned kInputFrames = 512;
// A core's sound-data input area in SPU2 RAM: a ring of 0x200 left samples, then 0x200 right samples. The mixer
// plays it at the output position, whoever wrote it; auto-DMA fills it a half (0x100 frames) at a time.
constexpr std::uint32_t kInputBase[2] = {0x2000u, 0x2400u};
constexpr std::uint32_t kInputHalf = 0x100;
// Mix switches of a core. VMIX*: one bit per voice, to the dry mix (L, R) and to the effects (EL, ER).
// MMIX, from bit 11 down: voices, input and external input (core 0's output, on core 1) into the core's
// output, each as dry left, dry right, effects left, effects right.
constexpr std::uint32_t kVmixl = 0x188, kVmixel = 0x18C, kVmixr = 0x190, kVmixer = 0x194, kMmix = 0x198;
// Reverb registers of a core. The work area is SPU2 RAM from ESA to the end of the 64K block EEA names.
// From kReverbAddress on, 22 offsets into the work area (two halfwords each, high first), named here as
// in the published description of the PlayStation SPU reverb: d = a source, m = a destination.
constexpr std::uint32_t kEsa = 0x2E0, kReverbAddress = 0x2E4, kEea = 0x33C;
enum ReverbAddress {
    kApf1Size, kApf2Size, kSameDst, kSameDstR, kComb1, kComb1R, kComb2, kComb2R, kSameSrc, kSameSrcR, kDiffDst, kDiffDstR,
    kComb3, kComb3R, kComb4, kComb4R, kDiffSrc, kDiffSrcR, kApf1Dst, kApf1DstR, kApf2Dst, kApf2DstR, kReverbAddresses
};
// The ten reverb gains follow the volumes in a core's volume block (0x774 for core 0, 0x79C for core 1).
enum ReverbGain { kIirGain, kComb1Gain, kComb2Gain, kComb3Gain, kComb4Gain, kWallGain, kApf1Gain, kApf2Gain, kInputGain, kInputGainR, kReverbGains };
// The reverb runs at half the sample rate, one side per output sample. This half-band filter halves the
// rate of its input and doubles the rate of its output (every second coefficient is 0).
constexpr int kReverbTaps = 39;
constexpr std::int32_t kReverbFilter[kReverbTaps] = {-1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384,
                                                     10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};
constexpr unsigned kReverbHistory = 64;
// The SPU's sample interpolation: four neighbouring samples weighted by this table, indexed by the top
// eight bits of the fraction of the read position i: kGauss[0xFF - i] for the oldest, [0x1FF - i],
// [0x100 + i], and [i] for the newest. The four weights of an i add up to 0x7F7F..0x7F81 of 0x8000.
constexpr std::int16_t kGauss[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};
constexpr unsigned kSpuLine = 9;
// The mixer's write-back areas in SPU2 RAM (halfword addresses). Every output sample each core
// stores voice 1 and voice 3 (after ADSR) and its dry and wet L/R voice mixes, and core 0 stores its
// final L/R output, each at area + output position (a 0x200-halfword ring). Software uses these as a
// clock by pointing IRQA into one of them (the address IRQ fires when the mixer writes there), and
// reads the mixes back by DMA to process them itself.
constexpr std::uint32_t kOutputRing = 0x200;
constexpr std::uint32_t kVoice1Area[2] = {0x0400u, 0x0C00u};
constexpr std::uint32_t kVoice3Area[2] = {0x0600u, 0x0E00u};
constexpr std::uint32_t kDryArea[2][2] = {{0x1000u, 0x1200u}, {0x1800u, 0x1A00u}};
constexpr std::uint32_t kWetArea[2][2] = {{0x1400u, 0x1600u}, {0x1C00u, 0x1E00u}};
constexpr std::uint32_t kCore0OutArea[2] = {0x0800u, 0x0A00u};
static_assert(kOutputRing % kBatch == 0, "a batch must not straddle the output ring wrap");

enum Phase { kOff, kAttack, kDecay, kSustain, kRelease };

struct Envelope {
    int counter = 0;
    std::int32_t level = 0;

    void step(int rate, bool decrease, bool exponential) {
        const int shift = rate >> 2;
        int cycles = 1 << std::max(0, shift - 11);
        if (exponential && !decrease && level > 0x6000) cycles *= 4;
        if (++counter < cycles) return;
        counter = 0;
        int delta = (decrease ? -8 + (rate & 3) : 7 - (rate & 3)) << std::max(0, 11 - shift);
        if (exponential && decrease) delta = delta * level / 0x8000;
        level = std::clamp(level + delta, 0, 0x7FFF);
    }
};

struct Voice {
    std::uint16_t voll = 0, volr = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
    Envelope vol[2];
    std::uint32_t ssa = 0, lsax = 0, next = 0, block[2] = {};
    bool lsax_written = false, key_on = false, end = false;
    Phase phase = kOff;
    Envelope env;
    std::int16_t buffer[56] = {};
    std::int32_t s1 = 0, s2 = 0;
    std::uint32_t position = 0;
};

struct Reverb {
    std::uint16_t gain[kReverbGains] = {};
    std::int16_t down[2][kReverbHistory] = {}, up[2][kReverbHistory] = {};   // [side]: input and output of the half-rate network
    std::uint32_t at = 0;                                                    // newest entry of the histories
};

struct Transfer {
    std::uint32_t madr = 0, bytes = 0;
    int channel = -1;
};

struct Core {
    Voice voices[24];
    std::uint16_t attr = 0, admas = 0;
    std::uint32_t irqa = 0x800u, tsa = 0;
    std::uint16_t mvol[2] = {0x3FFF, 0x3FFF}, bvol[2] = {0x7FFF, 0x7FFF}, avol[2] = {0x7FFF, 0x7FFF}, evol[2] = {};
    bool irq = false;
    std::int16_t input[kInputFrames][2] = {};   // not used any more; kept, with input_read, so saved states keep their layout
    unsigned input_read = 0, input_count = 0;   // input_count: frames ahead of the output position that auto-DMA has filled
    std::deque<Transfer> transfers;
    std::uint16_t regs[0x200] = {};
    Reverb reverb;
};

std::vector<std::uint16_t> g_ram(kRamWords);
Core g_cores[2];
std::uint16_t g_spdif_out = 0, g_spdif_mode = 0, g_spdif_media = 0;
std::vector<std::int16_t> g_audio;
bool g_record = false;
std::function<void(const std::int16_t *, std::size_t)> g_sink;
std::uint64_t g_next_tick = 0;
bool g_tick_pending = false;
std::uint32_t g_out_pos = 0;
std::uint32_t g_samples = 0;   // output samples since reset: the reverb's clock

void check_irq(std::uint32_t address, std::uint32_t words) {
    for (Core &k : g_cores) {
        if (!(k.attr & 0x40u) || k.irq) continue;
        if (((k.irqa - address) & kRamMask) >= words) continue;
        k.irq = true;
        static const bool trace = std::getenv("PS2_TRACE_SPU2") != nullptr;
        if (trace) std::fprintf(stderr, "[spu2] core %d irq at 0x%05X (access 0x%05X+%u) time %llu\n", static_cast<int>(&k - g_cores), k.irqa, address, words,
                                static_cast<unsigned long long>(kernel::now()));
        hw::raise_interrupt(kSpuLine);
    }
}

void decode_block(Voice &v, std::int16_t *out) {
    static const int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    const std::uint32_t at = v.next & kRamMask;
    const std::uint16_t header = g_ram[at];
    const int shift = header & 0xF, filter = std::min((header >> 4) & 0xF, 4);
    const unsigned flags = header >> 8;
    for (int i = 0; i < 28; ++i) {
        const std::uint16_t word = g_ram[(at + 1 + i / 4) & kRamMask];
        std::int32_t sample = static_cast<std::int16_t>(((word >> ((i & 3) * 4)) & 0xF) << 12) >> shift;
        sample += (v.s1 * kFilter[filter][0] + v.s2 * kFilter[filter][1] + 32) >> 6;
        sample = std::clamp(sample, -32768, 32767);
        v.s2 = v.s1;
        v.s1 = sample;
        out[i] = static_cast<std::int16_t>(sample);
    }
    v.block[out == v.buffer ? 0 : 1] = at;
    if ((flags & 4u) && !v.lsax_written) v.lsax = at;
    v.next = (at + 8) & kRamMask;
    if (flags & 1u) {
        v.end = true;
        v.next = v.lsax;
        if (!(flags & 2u)) {
            v.phase = kRelease;
            v.env.level = 0;
        }
    }
}

void key_on(Voice &v) {
    v.key_on = false;
    v.lsax_written = false;
    v.end = false;
    v.next = v.ssa;
    v.s1 = v.s2 = 0;
    v.position = 0;
    v.phase = kAttack;
    v.env = Envelope{};
    decode_block(v, v.buffer);
    decode_block(v, v.buffer + 28);
    check_irq(v.block[0], 8);
}

void update_envelope(Voice &v) {
    Envelope &e = v.env;
    switch (v.phase) {
    case kAttack:
        e.step((v.adsr1 >> 8) & 0x7F, false, v.adsr1 & 0x8000u);
        if (e.level >= 0x7FFF) v.phase = kDecay;
        break;
    case kDecay:
        e.step(((v.adsr1 >> 4) & 0xF) << 2, true, true);
        if (e.level <= ((v.adsr1 & 0xF) + 1) * 0x800) v.phase = kSustain;
        break;
    case kSustain:
        e.step((v.adsr2 >> 6) & 0x7F, v.adsr2 & 0x4000u, v.adsr2 & 0x8000u);
        break;
    case kRelease:
        e.step((v.adsr2 & 0x1F) << 2, true, v.adsr2 & 0x20u);
        if (e.level <= 0) v.phase = kOff;
        break;
    default:
        break;
    }
}

std::int32_t volume(std::uint16_t reg, Envelope &e) {
    if (!(reg & 0x8000u)) return static_cast<std::int16_t>(reg << 1);
    e.step(reg & 0x7F, reg & 0x2000u, reg & 0x4000u);
    return e.level;
}

// One batch of a voice: its left and right samples after the envelope and the voice volumes. The volume
// sweeps step every sample whether the voice plays or not.
void render_voice(Voice &v, std::int32_t (*voice)[2], std::int16_t *capture) {
    if (v.key_on) key_on(v);
    const std::uint32_t step = std::min<std::uint32_t>(v.pitch, 0x3FFFu);
    const bool sweep_l = v.voll & 0x8000u, sweep_r = v.volr & 0x8000u;
    const std::int32_t fixed_l = static_cast<std::int16_t>(v.voll << 1), fixed_r = static_cast<std::int16_t>(v.volr << 1);
    std::uint32_t position = v.position;
    for (unsigned n = 0; n < kBatch; ++n) {
        const std::int32_t left = sweep_l ? volume(v.voll, v.vol[0]) : fixed_l, right = sweep_r ? volume(v.volr, v.vol[1]) : fixed_r;
        if (v.phase == kOff) continue;
        const std::uint32_t phase = (position >> 4) & 0xFFu;
        const std::int16_t *const s = v.buffer + (position >> 12);   // four samples from the read position on: the block after is decoded already
        const std::int32_t sample = (kGauss[0xFF - phase] * s[0] >> 15) + (kGauss[0x1FF - phase] * s[1] >> 15) + (kGauss[0x100 + phase] * s[2] >> 15) + (kGauss[phase] * s[3] >> 15);
        position += step;
        while ((position >> 12) >= 28) {
            position -= 28u << 12;
            std::memmove(v.buffer, v.buffer + 28, 28 * sizeof(std::int16_t));
            v.block[0] = v.block[1];
            v.position = position;
            check_irq(v.block[0], 8);
            decode_block(v, v.buffer + 28);
        }
        update_envelope(v);
        const std::int32_t out = sample * v.env.level >> 15;
        if (capture) capture[n] = static_cast<std::int16_t>(std::clamp(out, -32768, 32767));
        voice[n][0] = out * left >> 15;
        voice[n][1] = out * right >> 15;
    }
    v.position = position;
}

// Auto-DMA: each block of a transfer (0x100 left samples, then 0x100 right) goes into the half of the
// input area that is not being played, one block per half.
void feed_input(int core) {
    Core &k = g_cores[core];
    const std::uint32_t rest = kInputHalf - g_out_pos % kInputHalf;   // frames left in the half being played
    if (k.transfers.empty() || k.input_count > rest) return;
    Transfer &t = k.transfers.front();
    const std::uint32_t chunk = std::min<std::uint32_t>(t.bytes, 1024);
    const std::uint32_t frames = std::max<std::uint32_t>(chunk / 4, 1);
    const std::uint32_t at = kInputBase[core] + (g_out_pos / kInputHalf ^ 1u) * kInputHalf;
    const std::uint8_t *src = ram_ptr(t.madr);
    for (std::uint32_t i = 0; i < frames; ++i) {
        std::memcpy(&g_ram[at + i], src + i * 2, 2);
        std::memcpy(&g_ram[at + kOutputRing + i], src + chunk / 2 + i * 2, 2);
    }
    k.input_count = rest + frames;
    t.madr += chunk;
    t.bytes -= chunk;
    if (!t.bytes) {
        const int channel = t.channel;
        k.transfers.pop_front();
        if (channel >= 0) kernel::schedule(kernel::now() + 32, kernel::Job{kernel::kJobDmaDone, static_cast<std::uint32_t>(channel)});
    }
}

std::int32_t master(std::uint16_t reg) { return reg & 0x8000u ? 0x7FFF : static_cast<std::int16_t>(reg << 1); }

std::int16_t clamp16(std::int32_t value) { return static_cast<std::int16_t>(std::clamp(value, -32768, 32767)); }

std::uint32_t switches(const Core &k, std::uint32_t off) { return k.regs[off >> 1] | static_cast<std::uint32_t>(k.regs[(off + 2) >> 1]) << 16; }

std::int32_t scale(std::int32_t value, std::int32_t gain) { return static_cast<std::int32_t>((static_cast<std::int64_t>(value) * gain) >> 15); }

std::int32_t reverb_filter(const std::int16_t *history, unsigned newest) {
    std::int32_t sum = kReverbFilter[kReverbTaps / 2] * history[(newest - kReverbTaps / 2) % kReverbHistory];
    for (int i = 0; i < kReverbTaps; i += 2) sum += kReverbFilter[i] * history[(newest - (kReverbTaps - 1) + i) % kReverbHistory];
    return sum;
}

// A core's reverb for one batch: in is its effects mix, out what comes back (before EVOL). Each output
// sample one side of the network runs, left on even samples: two reflection filters (same side, and
// fed from the other side), four combs and two all-pass filters, all in the work area, whose origin
// moves on one halfword every two samples. The network always runs and is heard; it writes to the work
// area, and tests the address IRQ there, only while the core's effects are enabled (ATTR bit 7).
void render_reverb(Core &k, const std::int32_t (*in)[2], std::int32_t (*out)[2]) {
    const std::uint32_t start = (static_cast<std::uint32_t>(k.regs[kEsa >> 1]) << 16 | k.regs[(kEsa + 2) >> 1]) & kRamMask;
    const std::uint32_t end = (static_cast<std::uint32_t>(k.regs[kEea >> 1]) << 16 | 0xFFFFu) & kRamMask;
    const bool enabled = k.attr & 0x80u;
    if (start >= end || (!enabled && !k.evol[0] && !k.evol[1])) return;
    const std::int64_t size = end - start + 1;
    std::int64_t offset[kReverbAddresses];
    for (int i = 0; i < kReverbAddresses; ++i)
        offset[i] = static_cast<std::uint32_t>(k.regs[(kReverbAddress + i * 4) >> 1]) << 16 | k.regs[(kReverbAddress + i * 4 + 2) >> 1];
    bool watched = false;
    for (const Core &other : g_cores) watched = watched || (enabled && (other.attr & 0x40u) && !other.irq && other.irqa >= start && other.irqa <= end);
    Reverb &r = k.reverb;
    std::int32_t gain[kReverbGains];
    for (int i = 0; i < kReverbGains; ++i) gain[i] = static_cast<std::int16_t>(r.gain[i]);
    for (unsigned n = 0; n < kBatch; ++n) {
        const std::uint32_t clock = g_samples + n;
        const int side = clock & 1;
        const std::int64_t origin = clock >> 1;
        const auto at = [&](std::int64_t relative) {
            const std::uint32_t address = start + static_cast<std::uint32_t>((((origin + relative) % size) + size) % size);
            if (watched) check_irq(address, 1);
            return address;
        };
        r.at = (r.at + 1) % kReverbHistory;
        r.down[0][r.at] = clamp16(in[n][0]);
        r.down[1][r.at] = clamp16(in[n][1]);
        const std::int32_t input = scale(clamp16(reverb_filter(r.down[side], r.at) >> 15), gain[kInputGain + side]);
        const auto sample = [&](std::uint32_t address) { return static_cast<std::int32_t>(static_cast<std::int16_t>(g_ram[address])); };
        const std::uint32_t same_dst = at(offset[kSameDst + side]), same_old = at(offset[kSameDst + side] - 1);
        const std::uint32_t diff_dst = at(offset[kDiffDst + side]), diff_old = at(offset[kDiffDst + side] - 1);
        const std::int32_t same = scale(input + scale(sample(at(offset[kSameSrc + side])), gain[kWallGain]) - sample(same_old), gain[kIirGain]) + sample(same_old);
        const std::int32_t diff = scale(input + scale(sample(at(offset[kDiffSrc + (side ^ 1)])), gain[kWallGain]) - sample(diff_old), gain[kIirGain]) + sample(diff_old);
        std::int32_t mix = 0;
        for (int comb = 0; comb < 4; ++comb) {
            static const int kSource[4] = {kComb1, kComb2, kComb3, kComb4};
            mix += scale(sample(at(offset[kSource[comb] + side])), gain[kComb1Gain + comb]);
        }
        const std::uint32_t apf1_dst = at(offset[kApf1Dst + side]), apf1_src = at(offset[kApf1Dst + side] - offset[kApf1Size]);
        const std::uint32_t apf2_dst = at(offset[kApf2Dst + side]), apf2_src = at(offset[kApf2Dst + side] - offset[kApf2Size]);
        const std::int32_t apf1 = mix - scale(sample(apf1_src), gain[kApf1Gain]);
        mix = sample(apf1_src) + scale(apf1, gain[kApf1Gain]);
        const std::int32_t apf2 = mix - scale(sample(apf2_src), gain[kApf2Gain]);
        mix = sample(apf2_src) + scale(apf2, gain[kApf2Gain]);
        if (enabled) {
            g_ram[same_dst] = static_cast<std::uint16_t>(clamp16(same));
            g_ram[diff_dst] = static_cast<std::uint16_t>(clamp16(diff));
            g_ram[apf1_dst] = static_cast<std::uint16_t>(clamp16(apf1));
            g_ram[apf2_dst] = static_cast<std::uint16_t>(clamp16(apf2));
        }
        r.up[side][r.at] = clamp16(mix);
        r.up[side ^ 1][r.at] = 0;
        out[n][0] = clamp16(reverb_filter(r.up[0], r.at) >> 14);
        out[n][1] = clamp16(reverb_filter(r.up[1], r.at) >> 14);
    }
}

void write_area(std::uint32_t area, const std::int16_t *values) {
    const std::uint32_t at = area + g_out_pos;
    check_irq(at, kBatch);
    for (unsigned n = 0; n < kBatch; ++n) g_ram[at + n] = static_cast<std::uint16_t>(values[n]);
}

void tick() {
    std::int16_t out[kBatch * 2];
    std::int16_t raw[2][2][kBatch] = {};   // [core][voice 1, voice 3][sample], 0 while the voice is off
    std::int16_t dry[2][2][kBatch];        // [core][left, right][sample], the voices switched to the dry mix
    std::int16_t wet[2][2][kBatch];        // the voices switched to the effects
    std::int16_t core0[2][kBatch] = {};    // core 0 output after its master volume: the external input of core 1
    static const bool count = std::getenv("PS2_SPU2_COUNT") != nullptr;
    static std::uint64_t ticks = 0, active = 0, phases[5] = {};
    for (int c = 0; c < 2; ++c) {
        Core &k = g_cores[c];
        const std::uint32_t to[4] = {switches(k, kVmixl), switches(k, kVmixr), switches(k, kVmixel), switches(k, kVmixer)};
        std::int32_t sum[kBatch][4] = {};   // dry left, dry right, effects left, effects right
        for (int i = 0; i < 24; ++i) {
            Voice &v = k.voices[i];
            const bool playing = v.key_on || v.phase != kOff;
            if (!playing && !((v.voll | v.volr) & 0x8000u)) continue;
            if (count && playing) {
                ++active;
                ++phases[v.phase];
            }
            std::int32_t voice[kBatch][2] = {};
            render_voice(v, voice, i == 1 ? raw[c][0] : i == 3 ? raw[c][1] : nullptr);
            if (!playing) continue;
            for (int m = 0; m < 4; ++m) {
                if (!(to[m] & (1u << i))) continue;
                for (unsigned n = 0; n < kBatch; ++n) sum[n][m] += voice[n][m & 1];
            }
        }
        for (unsigned n = 0; n < kBatch; ++n)
            for (int side = 0; side < 2; ++side) {
                dry[c][side][n] = clamp16(sum[n][side]);
                wet[c][side][n] = clamp16(sum[n][2 + side]);
            }
        const std::uint32_t input = kInputBase[c] + g_out_pos;
        check_irq(input, kBatch);
        check_irq(input + kOutputRing, kBatch);
        const std::uint16_t mmix = k.regs[kMmix >> 1];
        std::int32_t mix[kBatch][2] = {}, effects[kBatch][2] = {}, returned[kBatch][2] = {};
        for (unsigned n = 0; n < kBatch; ++n)
            for (int side = 0; side < 2; ++side) {
                const std::int32_t sources[3] = {dry[c][side][n], static_cast<std::int16_t>(g_ram[input + side * kOutputRing + n]) * static_cast<std::int16_t>(k.bvol[side]) >> 15,
                                                 c == 1 ? core0[side][n] * static_cast<std::int16_t>(k.avol[side]) >> 15 : 0};
                for (int source = 0; source < 3; ++source) {
                    if (mmix & (0x800u >> (source * 4 + side))) mix[n][side] += sources[source];
                    if (mmix & (0x200u >> (source * 4 + side))) effects[n][side] += source ? sources[source] : wet[c][side][n];
                }
            }
        render_reverb(k, effects, returned);
        for (unsigned n = 0; n < kBatch; ++n)
            for (int side = 0; side < 2; ++side) {
                const std::int32_t sum = (mix[n][side] + scale(returned[n][side], static_cast<std::int16_t>(k.evol[side]))) * master(k.mvol[side]) >> 15;
                if (c == 0) core0[side][n] = clamp16(sum);
                else out[n * 2 + side] = clamp16(sum);
            }
    }
    if (count && ++ticks % 15000 == 0)
        std::fprintf(stderr, "[spu2] %.1f voices per batch (attack %.1f decay %.1f sustain %.1f release %.1f)\n", static_cast<double>(active) / ticks,
                     static_cast<double>(phases[1]) / ticks, static_cast<double>(phases[2]) / ticks, static_cast<double>(phases[3]) / ticks, static_cast<double>(phases[4]) / ticks);
    if (g_record) g_audio.insert(g_audio.end(), out, out + kBatch * 2);
    if (g_sink) g_sink(out, kBatch);
    for (int c = 0; c < 2; ++c) {
        write_area(kVoice1Area[c], raw[c][0]);
        write_area(kVoice3Area[c], raw[c][1]);
        write_area(kDryArea[c][0], dry[c][0]);
        write_area(kDryArea[c][1], dry[c][1]);
        write_area(kWetArea[c][0], wet[c][0]);
        write_area(kWetArea[c][1], wet[c][1]);
        if (c == 0) {
            write_area(kCore0OutArea[0], core0[0]);
            write_area(kCore0OutArea[1], core0[1]);
        }
        g_cores[c].input_count -= std::min(g_cores[c].input_count, kBatch);
    }
    g_out_pos = (g_out_pos + kBatch) % kOutputRing;
    g_samples += kBatch;
    feed_input(0);
    feed_input(1);
    g_next_tick += kBatch * kCyclesPerSample;
    kernel::schedule(g_next_tick, kernel::Job{kernel::kJobSpuTick});
}

void keep_ticking() {
    if (g_tick_pending) return;
    g_tick_pending = true;
    g_next_tick = kernel::now() + kBatch * kCyclesPerSample;
    kernel::schedule(g_next_tick, kernel::Job{kernel::kJobSpuTick});
}

std::uint32_t set_hi(std::uint32_t address, std::uint32_t v) { return ((v & 0xFu) << 16) | (address & 0xFFFFu); }
std::uint32_t set_lo(std::uint32_t address, std::uint32_t v) { return (address & 0xF0000u) | (v & 0xFFFFu); }

std::uint32_t read_voice(Core &k, std::uint32_t off) {
    if (off < 0x180u) {
        const Voice &v = k.voices[off >> 4];
        switch (off & 0xFu) {
        case 0x0: return v.voll;
        case 0x2: return v.volr;
        case 0x4: return v.pitch;
        case 0x6: return v.adsr1;
        case 0x8: return v.adsr2;
        case 0xA: return static_cast<std::uint32_t>(v.env.level);
        case 0xC: return static_cast<std::uint32_t>(v.vol[0].level);
        default: return static_cast<std::uint32_t>(v.vol[1].level);
        }
    }
    const Voice &v = k.voices[(off - 0x1C0u) / 12];
    const std::uint32_t nax = (v.block[0] + 1 + (v.position >> 14)) & kRamMask;
    switch ((off - 0x1C0u) % 12) {
    case 0: return v.ssa >> 16;
    case 2: return v.ssa & 0xFFFFu;
    case 4: return v.lsax >> 16;
    case 6: return v.lsax & 0xFFFFu;
    case 8: return nax >> 16;
    default: return nax & 0xFFFFu;
    }
}

void write_voice(Core &k, std::uint32_t off, std::uint32_t value) {
    if (off < 0x180u) {
        Voice &v = k.voices[off >> 4];
        switch (off & 0xFu) {
        case 0x0:
            v.voll = static_cast<std::uint16_t>(value);
            if (!(value & 0x8000u)) v.vol[0].level = static_cast<std::int16_t>(value << 1);
            break;
        case 0x2:
            v.volr = static_cast<std::uint16_t>(value);
            if (!(value & 0x8000u)) v.vol[1].level = static_cast<std::int16_t>(value << 1);
            break;
        case 0x4: v.pitch = static_cast<std::uint16_t>(value); break;
        case 0x6: v.adsr1 = static_cast<std::uint16_t>(value); break;
        case 0x8: v.adsr2 = static_cast<std::uint16_t>(value); break;
        case 0xA: v.env.level = static_cast<std::int16_t>(value); break;
        default: break;
        }
        return;
    }
    Voice &v = k.voices[(off - 0x1C0u) / 12];
    switch ((off - 0x1C0u) % 12) {
    case 0: v.ssa = set_hi(v.ssa, value); break;
    case 2: v.ssa = set_lo(v.ssa, value); break;
    case 4: v.lsax = set_hi(v.lsax, value); v.lsax_written = true; break;
    case 6: v.lsax = set_lo(v.lsax, value); v.lsax_written = true; break;
    case 8: v.next = set_hi(v.next, value); break;
    default: v.next = set_lo(v.next, value); break;
    }
}

void key(Core &k, std::uint32_t mask, bool on) {
    for (int i = 0; i < 24; ++i) {
        if (!(mask & (1u << i))) continue;
        Voice &v = k.voices[i];
        if (on) v.key_on = true;
        else if (v.key_on) v.key_on = false;
        else if (v.phase != kOff) v.phase = kRelease;
    }
}

std::uint32_t read_core(int core, std::uint32_t off) {
    Core &k = g_cores[core];
    if (off < 0x180u || (off >= 0x1C0u && off < 0x2E0u)) return read_voice(k, off);
    switch (off) {
    case 0x19A: return k.attr;
    case 0x19C: return k.irqa >> 16;
    case 0x19E: return k.irqa & 0xFFFFu;
    case 0x1A8: return k.tsa >> 16;
    case 0x1AA: return k.tsa & 0xFFFFu;
    case 0x1B0: return k.admas;
    case 0x340:
    case 0x342: {
        std::uint32_t endx = 0;
        for (int i = 0; i < 24; ++i)
            if (k.voices[i].end) endx |= 1u << i;
        return off == 0x340 ? endx & 0xFFFFu : endx >> 16;
    }
    case 0x344: return k.attr & 0x30u ? 0x80u : 0u;
    default: return k.regs[off >> 1];
    }
}

void write_core(int core, std::uint32_t off, std::uint32_t value) {
    Core &k = g_cores[core];
    static const bool trace = std::getenv("PS2_TRACE_SPU2") != nullptr;
    if (trace && off >= 0x180u && !(off >= 0x1C0u && off < 0x2E0u) && off != 0x1AC)
        std::fprintf(stderr, "[spu2] core %d reg 0x%03X = 0x%04X\n", core, off, value);
    if (off < 0x180u || (off >= 0x1C0u && off < 0x2E0u)) {
        write_voice(k, off, value);
        return;
    }
    k.regs[off >> 1] = static_cast<std::uint16_t>(value);
    switch (off) {
    case 0x19A:
        k.attr = static_cast<std::uint16_t>(value);
        if (!(value & 0x40u)) k.irq = false;
        break;
    case 0x19C: k.irqa = set_hi(k.irqa, value); break;
    case 0x19E: k.irqa = set_lo(k.irqa, value); break;
    case 0x1A0: key(k, value, true); break;
    case 0x1A2: key(k, value << 16, true); break;
    case 0x1A4: key(k, value, false); break;
    case 0x1A6: key(k, value << 16, false); break;
    case 0x1A8: k.tsa = set_hi(k.tsa, value); break;
    case 0x1AA: k.tsa = set_lo(k.tsa, value); break;
    case 0x1AC:
        check_irq(k.tsa, 1);
        g_ram[k.tsa] = static_cast<std::uint16_t>(value);
        k.tsa = (k.tsa + 1) & kRamMask;
        break;
    case 0x1B0:
        k.admas = static_cast<std::uint16_t>(value);
        if (!value) k.input_count = k.input_read = 0;
        break;
    case 0x340:
    case 0x342:
        for (Voice &v : k.voices) v.end = false;
        break;
    default: break;
    }
}

std::uint16_t *master_reg(int core, std::uint32_t off) {
    Core &k = g_cores[core];
    switch (off) {
    case 0x0: return &k.mvol[0];
    case 0x2: return &k.mvol[1];
    case 0x4: return &k.evol[0];
    case 0x6: return &k.evol[1];
    case 0x8: return &k.avol[0];
    case 0xA: return &k.avol[1];
    case 0xC: return &k.bvol[0];
    case 0xE: return &k.bvol[1];
    default: return off >= 0x14u ? &k.reverb.gain[(off - 0x14u) >> 1] : nullptr;
    }
}

std::uint32_t read_global(std::uint32_t off) {
    switch (off) {
    case 0x7C0: return g_spdif_out;
    case 0x7C2: return (g_cores[0].irq ? 4u : 0u) | (g_cores[1].irq ? 8u : 0u);
    case 0x7C6: return g_spdif_mode;
    case 0x7C8: return g_spdif_media;
    default: return 0;
    }
}

void write_global(std::uint32_t off, std::uint32_t value) {
    switch (off) {
    case 0x7C0: g_spdif_out = static_cast<std::uint16_t>(value); break;
    case 0x7C6: g_spdif_mode = static_cast<std::uint16_t>(value); break;
    case 0x7C8: g_spdif_media = static_cast<std::uint16_t>(value); break;
    default: break;
    }
}

}

void reset() {
    kernel::register_job(kernel::kJobSpuTick, [](const kernel::Job &) { tick(); });
    std::fill(g_ram.begin(), g_ram.end(), 0);
    for (Core &k : g_cores) {
        k = Core{};
        for (std::uint32_t off : {kVmixl, kVmixel, kVmixr, kVmixer}) {
            k.regs[off >> 1] = 0xFFFF;
            k.regs[(off + 2) >> 1] = 0x00FF;
        }
        k.regs[kMmix >> 1] = 0xFFCF;
        k.regs[kEsa >> 1] = k.regs[kEea >> 1] = static_cast<std::uint16_t>(0xE + (&k - g_cores));
        k.regs[(kEsa + 2) >> 1] = 0xFFF8;
    }
    g_samples = 0;
    g_spdif_out = g_spdif_mode = g_spdif_media = 0;
    g_audio.clear();
    g_record = std::getenv("PS2_DUMP_AUDIO") != nullptr;
    g_next_tick = 0;
    g_tick_pending = false;
    g_out_pos = 0;
}

std::uint16_t *ram() { return g_ram.data(); }

std::uint32_t read(std::uint32_t a, unsigned size) {
    keep_ticking();
    const std::uint32_t off = a & 0x7FEu;
    std::uint32_t value = 0;
    if (off < 0x760u) value = read_core((off & 0x400u) ? 1 : 0, off & 0x3FEu);
    else if (off < 0x7B0u) {
        if (const std::uint16_t *reg = master_reg((off - 0x760u) / 0x28, (off - 0x760u) % 0x28)) value = *reg;
    } else value = read_global(off);
    if (size == 4) value |= read(a + 2, 2) << 16;
    return value;
}

void write(std::uint32_t a, std::uint32_t v, unsigned size) {
    keep_ticking();
    if (size == 4) {
        write(a, v & 0xFFFFu, 2);
        write(a + 2, v >> 16, 2);
        return;
    }
    const std::uint32_t off = a & 0x7FEu;
    v &= 0xFFFFu;
    if (off < 0x760u) write_core((off & 0x400u) ? 1 : 0, off & 0x3FEu, v);
    else if (off < 0x7B0u) {
        static const bool trace = std::getenv("PS2_TRACE_SPU2") != nullptr;
        if (trace) std::fprintf(stderr, "[spu2] core %u volume block 0x%02X = 0x%04X\n", (off - 0x760u) / 0x28, (off - 0x760u) % 0x28, v);
        if (std::uint16_t *reg = master_reg((off - 0x760u) / 0x28, (off - 0x760u) % 0x28)) *reg = static_cast<std::uint16_t>(v);
    } else write_global(off, v);
}

void dma(int core, std::uint32_t madr, std::uint32_t words, bool to_spu, int channel) {
    keep_ticking();
    Core &k = g_cores[core];
    static const bool trace = std::getenv("PS2_TRACE_SPU2") != nullptr;
    if (trace)
        std::fprintf(stderr, "[spu2] core %d dma %s 0x%05X, %u halfwords%s, output position 0x%03X\n", core, to_spu ? "write to" : "read from", k.tsa, words * 2,
                     to_spu && (k.admas & (1u << core)) ? " (auto)" : "", g_out_pos);
    if (to_spu && (k.admas & (1u << core))) {
        k.transfers.push_back(Transfer{madr, words * 4, channel});
        feed_input(core);
        return;
    }
    const std::uint32_t halfwords = words * 2;
    if (to_spu) check_irq(k.tsa, halfwords);
    for (std::uint32_t i = 0; i < halfwords; ++i) {
        std::uint16_t *spu = &g_ram[(k.tsa + i) & kRamMask];
        std::uint8_t *iop = ram_ptr(madr + i * 2);
        if (to_spu) std::memcpy(spu, iop, 2);
        else std::memcpy(iop, spu, 2);
    }
    k.tsa = (k.tsa + halfwords) & kRamMask;
    if (channel >= 0) kernel::schedule(kernel::now() + 64 + words / 4, kernel::Job{kernel::kJobDmaDone, static_cast<std::uint32_t>(channel)});
}

void set_sink(std::function<void(const std::int16_t *, std::size_t)> sink) { g_sink = std::move(sink); }

void dump_voices() {
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 24; ++i) {
            const Voice &v = g_cores[c].voices[i];
            if (v.phase == kOff && !v.end) continue;
            const std::uint16_t header = g_ram[v.block[0] & kRamMask];
            std::fprintf(stderr, "[spu2] core %d voice %d phase %d ssa 0x%05X lsax 0x%05X block 0x%05X flags %X next 0x%05X pitch 0x%04X env %d end %d\n", c, i,
                         static_cast<int>(v.phase), v.ssa, v.lsax, v.block[0], header >> 8, v.next, v.pitch, v.env.level, v.end ? 1 : 0);
        }
}

void dump_audio(const char *path) {
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return;
    const std::uint32_t bytes = static_cast<std::uint32_t>(g_audio.size() * 2);
    const std::uint32_t header[] = {0x46464952u, 36 + bytes, 0x45564157u, 0x20746D66u, 16,         0x00020001u,
                                    48000,       48000 * 4,  0x00100004u, 0x61746164u, bytes};
    std::fwrite(header, 4, sizeof header / 4, f);
    std::fwrite(g_audio.data(), 2, g_audio.size(), f);
    std::fclose(f);
}

}

namespace ps2::iop::spu2 {

void state(ps2::state::Stream &s) {
    s.tag("spu2");
    s.vec(g_ram);
    for (Core &k : g_cores) {
        s.pod(k.voices);
        s.pod(k.attr);
        s.pod(k.admas);
        s.pod(k.irqa);
        s.pod(k.tsa);
        s.pod(k.mvol);
        s.pod(k.bvol);
        s.pod(k.avol);
        s.pod(k.evol);
        s.pod(k.irq);
        s.pod(k.input);
        s.pod(k.input_read);
        s.pod(k.input_count);
        s.deque(k.transfers, [&s](Transfer &t) {
            s.pod(t.madr);
            s.pod(t.bytes);
            s.pod(t.channel);
        });
        s.pod(k.regs);
        if (s.version >= 5) s.pod(k.reverb);
        else if (s.loading()) k.reverb = Reverb{};
    }
    s.pod(g_spdif_out);
    s.pod(g_spdif_mode);
    s.pod(g_spdif_media);
    s.pod(g_next_tick);
    s.pod(g_tick_pending);
    s.pod(g_out_pos);
    if (s.version >= 5) s.pod(g_samples);
    else if (s.loading()) g_samples = 0;
}

}
