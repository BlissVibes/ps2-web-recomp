#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::vif {

void state(ps2::state::Stream &s);

void reset();
void reset_vu_side();
void stats();
void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc);
void transfer_words(int unit, const std::uint32_t *words, std::size_t count);   // the same in words: the VU thread's kVif1
bool stalled(int unit);
void describe();   // one line to stderr: stalls and pending data of each unit
std::uint32_t read(int unit, std::uint32_t reg);
void write(int unit, std::uint32_t reg, std::uint32_t value);

}
