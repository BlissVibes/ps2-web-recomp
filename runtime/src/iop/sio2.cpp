#include "sio2.hpp"

#include "hw.hpp"
#include "iop.hpp"

#include <cstring>
#include <deque>
#include <vector>

namespace ps2::iop::sio2 {

namespace {

constexpr std::uint8_t kDigital = 0x41, kAnalog = 0x73, kPressure = 0x79, kConfig = 0xF3;
constexpr std::uint32_t kNoDevice = 0x1D100u;

struct Pad {
    bool connected = false;
    std::uint16_t buttons = 0;
    std::uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
    std::uint8_t mode = kDigital;
    bool config = false;
    bool locked = false;
    std::uint8_t poll_mask[3] = {0xFF, 0xFF, 0x03};
};

struct Dma {
    bool armed = false;
    std::uint32_t madr = 0, block_words = 0, blocks = 0;
};

std::uint32_t g_send3[16];
std::uint32_t g_ctrl1[4], g_ctrl2[4];
std::uint32_t g_index = 0;
std::uint32_t g_stat = 0;
std::deque<std::uint8_t> g_in, g_out;
Pad g_pads[2];
Dma g_dma[2];

void controller(int port, std::size_t at, std::uint32_t out_size) {
    if (port > 1 || !g_pads[port].connected) {
        g_stat = kNoDevice;
        return;
    }
    Pad &p = g_pads[port];
    const std::vector<std::uint8_t> in(g_in.begin(), g_in.end());
    const std::uint8_t cmd = in.size() > 1 ? in[1] : 0;
    auto put = [&](std::size_t i, std::uint8_t v) {
        if (i < out_size) g_out[at + i] = v;
    };
    put(0, 0xFF);
    put(1, p.config ? kConfig : p.mode);
    put(2, 0x5A);
    const std::uint16_t raw = static_cast<std::uint16_t>(~p.buttons);
    auto data = [&]() {
        put(3, static_cast<std::uint8_t>(raw));
        put(4, static_cast<std::uint8_t>(raw >> 8));
        if (p.mode != kDigital) {
            put(5, p.axes[0]);
            put(6, p.axes[1]);
            put(7, p.axes[2]);
            put(8, p.axes[3]);
        }
        if (p.mode == kPressure) {
            static const int kBits[12] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
            for (int i = 0; i < 12; ++i) put(9 + static_cast<std::size_t>(i), (p.buttons >> kBits[i]) & 1u ? 0xFF : 0x00);
        }
    };
    switch (cmd) {
    case 0x40:
        put(3, 0); put(4, 0); put(5, 2); put(6, 0); put(7, 0); put(8, 0x5A);
        break;
    case 0x41:
        if (p.mode == kDigital) for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0);
        else { put(3, p.poll_mask[0]); put(4, p.poll_mask[1]); put(5, p.poll_mask[2]); put(6, 0); put(7, 0); put(8, 0x5A); }
        break;
    case 0x42:
        data();
        break;
    case 0x43:
        if (!p.config) data();
        else for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0);
        if (in.size() > 3) p.config = in[3] == 1;
        break;
    case 0x44:
        if (in.size() > 4) {
            p.mode = in[3] ? kAnalog : kDigital;
            p.locked = in[4] == 3;
        }
        for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0);
        break;
    case 0x45:
        put(3, 0x03); put(4, 0x02); put(5, p.mode == kDigital ? 0 : 1); put(6, 0x02); put(7, 0x01); put(8, 0x00);
        break;
    case 0x46:
        put(3, 0);
        if (in.size() > 3 && in[3] == 0) { put(4, 0x00); put(5, 0x01); put(6, 0x02); put(7, 0x00); put(8, 0x0A); }
        else { put(4, 0x00); put(5, 0x01); put(6, 0x01); put(7, 0x01); put(8, 0x14); }
        break;
    case 0x47:
        put(3, 0); put(4, 0x00); put(5, 0x02); put(6, 0x00); put(7, 0x01); put(8, 0x00);
        break;
    case 0x4C:
        put(3, 0);
        if (in.size() > 3 && in[3] == 0) { put(4, 0); put(5, 0); put(6, 0x04); put(7, 0); put(8, 0); }
        else { put(4, 0); put(5, 0); put(6, 0x07); put(7, 0); put(8, 0); }
        break;
    case 0x4D:
        for (int i = 3; i < 9; ++i) put(static_cast<std::size_t>(i), 0xFF);
        break;
    case 0x4F:
        if (in.size() > 5) {
            p.poll_mask[0] = in[3];
            p.poll_mask[1] = in[4];
            p.poll_mask[2] = in[5];
        }
        p.mode = kPressure;
        put(3, 0); put(4, 0); put(5, 0); put(6, 0); put(7, 0); put(8, 0x5A);
        break;
    default:
        data();
        break;
    }
}

void process() {
    const std::uint32_t reg = g_send3[g_index & 15u];
    const std::uint32_t in_size = (reg >> 8) & 0x1FFu, out_size = (reg >> 18) & 0x1FFu;
    if (g_in.size() < in_size) return;
    const int port = static_cast<int>(reg & 3u);
    const std::size_t at = g_out.size();
    for (std::uint32_t i = 0; i < out_size; ++i) g_out.push_back(0xFF);
    g_stat = 0x1100u;
    const std::uint32_t device = g_ctrl2[port];
    if (device == 0x5FFFFu || device == 0x00030064u || port >= 2) g_stat = kNoDevice;
    else controller(port, at, out_size);
    g_in.clear();
    ++g_index;
}

void run_transfer() {
    Dma &in = g_dma[0], &out = g_dma[1];
    if (in.armed) {
        for (std::uint32_t b = 0; b < in.blocks; ++b) {
            for (std::uint32_t i = 0; i < in.block_words * 4; ++i) g_in.push_back(read8(in.madr + b * in.block_words * 4 + i));
            process();
        }
        in.armed = false;
        hw::dma_done(11);
    }
    if (out.armed) {
        for (std::uint32_t b = 0; b < out.blocks && b < 16; ++b) {
            const std::uint32_t size = (g_send3[b] >> 18) & 0x1FFu;
            for (std::uint32_t i = 0; i < size && !g_out.empty(); ++i) {
                write8(out.madr + b * out.block_words * 4 + i, g_out.front());
                g_out.pop_front();
            }
        }
        out.armed = false;
        hw::dma_done(12);
    }
    hw::raise_interrupt(17);
}

}

void reset() {
    std::memset(g_send3, 0, sizeof g_send3);
    std::memset(g_ctrl1, 0, sizeof g_ctrl1);
    std::memset(g_ctrl2, 0, sizeof g_ctrl2);
    g_index = 0;
    g_stat = 0;
    g_in.clear();
    g_out.clear();
    g_pads[0] = Pad{};
    g_pads[1] = Pad{};
    g_pads[0].connected = true;
    g_dma[0] = g_dma[1] = Dma{};
}

std::uint32_t read(std::uint32_t a) {
    if (a >= 0x1F808200u && a < 0x1F808240u) return g_send3[(a - 0x1F808200u) / 4];
    if (a >= 0x1F808240u && a < 0x1F808260u) {
        const std::uint32_t port = (a - 0x1F808240u) / 8;
        return (a & 4u) ? g_ctrl2[port] : g_ctrl1[port];
    }
    switch (a) {
    case 0x1F808264u: {
        if (g_out.empty()) return 0xFF;
        const std::uint8_t v = g_out.front();
        g_out.pop_front();
        return v;
    }
    case 0x1F808268u: return 0;
    case 0x1F80826Cu: return g_stat;
    case 0x1F808270u: return 0xF;
    case 0x1F808274u: return 0;
    default: return 0;
    }
}

void write(std::uint32_t a, std::uint32_t v) {
    if (a >= 0x1F808200u && a < 0x1F808240u) { g_send3[(a - 0x1F808200u) / 4] = v; return; }
    if (a >= 0x1F808240u && a < 0x1F808260u) {
        const std::uint32_t port = (a - 0x1F808240u) / 8;
        if (a & 4u) g_ctrl2[port] = v;
        else g_ctrl1[port] = v;
        return;
    }
    switch (a) {
    case 0x1F808260u:
        g_in.push_back(static_cast<std::uint8_t>(v));
        process();
        break;
    case 0x1F808268u:
        if ((v & 0xCu) == 0xCu) {
            g_index = 0;
            g_in.clear();
            g_out.clear();
        }
        if (v & 1u) run_transfer();
        break;
    default:
        break;
    }
}

void arm_dma(int channel, std::uint32_t madr, std::uint32_t block_words, std::uint32_t blocks) {
    Dma &d = g_dma[channel == 11 ? 0 : 1];
    d.armed = true;
    d.madr = madr;
    d.block_words = block_words;
    d.blocks = blocks;
}

void set_buttons(int pad, std::uint16_t pressed_mask) {
    if (pad >= 0 && pad < 2) g_pads[pad].buttons = pressed_mask;
}

void set_axis(int pad, int axis, std::uint8_t value) {
    if (pad >= 0 && pad < 2 && axis >= 0 && axis < 4) g_pads[pad].axes[axis] = value;
}

}
