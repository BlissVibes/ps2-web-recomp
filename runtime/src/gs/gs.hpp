#pragma once

#include "ps2/cpu.hpp"

#include <cstdint>
#include <vector>

namespace ps2::gs {

inline constexpr std::uint32_t kMemorySize = 4u << 20;

enum Psm : std::uint32_t {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A,
};

struct Vertex {
    std::uint32_t x, y, z;
    std::uint8_t r, g, b, a;
    float q, s, t;
    std::uint32_t u, v;
    std::uint8_t f;
};

struct Context {
    std::uint64_t tex0, clamp, tex1, tex2, xyoffset, miptbp1, miptbp2, scissor, alpha, test, fba, frame, zbuf;
};

struct State {
    std::uint64_t prim, prmode, prmodecont, rgbaq, st, uv, fog, texclut, scanmsk, texa, fogcol, dimx, dthe, colclamp, pabe;
    std::uint64_t bitbltbuf, trxpos, trxreg, trxdir;
    Context ctx[2];
    float q;
    Vertex queue[3];
    int count;
    std::uint32_t clut[512];
};

extern State g_state;
std::uint8_t *memory();

void reset();
void write(std::uint32_t reg, std::uint64_t value);
void write_packed(std::uint32_t reg, const Reg128 &q);
void write_image(const std::uint8_t *data, std::uint32_t qwords);
void read_image(std::uint8_t *data, std::uint32_t qwords);

std::uint64_t read_privileged(std::uint32_t address);
void write_privileged(std::uint32_t address, std::uint64_t value);
void vblank(bool start);
bool interlaced_odd_field();

std::uint32_t address32(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y);
std::uint32_t read_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y);
void write_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t value);

bool display(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
std::uint64_t primitives_drawn();

}
