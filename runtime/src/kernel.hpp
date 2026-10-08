#pragma once

#include "ps2/cpu.hpp"

#include <cstdint>

namespace ps2::kernel {

void reset();
void start_main_thread(std::uint32_t entry, std::uint32_t gp, std::uint32_t argc, std::uint32_t argv);
void handle_syscall(Cpu &c, std::uint32_t pc);
void service(Cpu &c);
bool must_unwind();
bool in_handler();
void on_vblank_start();
void on_hblank();
void wake_idle();
void dump();

}
