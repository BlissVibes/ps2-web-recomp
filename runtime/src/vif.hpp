#pragma once

#include <cstdint>

namespace ps2::vif {

void reset();
void stats();
void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc);
std::uint32_t read(int unit, std::uint32_t reg);
void write(int unit, std::uint32_t reg, std::uint32_t value);

}
