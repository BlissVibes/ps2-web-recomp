#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::gif {

void state(ps2::state::Stream &s);

void reset();
void stats();
void transfer(int path, const std::uint8_t *data, std::uint32_t qwc);
void render(int path, const std::uint8_t *data, std::uint32_t qwc);
void render_reset();
void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc);
bool busy(int path);
void set_path3_mask(bool masked);   // VIF1 MSKPATH3
void path3_advance();               // time has passed on the EE side: an unmasked PATH3 drains what it holds
bool path3_advance_releases();      // whether path3_advance() would let held data go now
bool path3_unmask_releases();       // whether set_path3_mask(false) would
std::uint32_t read(std::uint32_t a);
void write(std::uint32_t a, std::uint32_t v);

}
