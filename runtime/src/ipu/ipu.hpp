#pragma once

#include <cstdint>

namespace ps2::ipu {

void reset();
std::uint32_t read32(std::uint32_t a);
void write32(std::uint32_t a, std::uint32_t v);
void write_fifo(const std::uint8_t *qword);
void read_fifo(std::uint8_t *qword);
void dma_started(int channel);
void stats();

}
