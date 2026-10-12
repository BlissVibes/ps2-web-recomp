#pragma once

#include "cpu.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ps2::harness {

// A buffer the harness places in the reserved stack area and points an argument register at.
struct Buffer {
    int reg;                                  // 0-7: a0-a3, t0-t3
    std::uint32_t size;
    std::vector<std::uint8_t> bytes;          // initial contents, zero-padded to size
};

// One call of a recompiled (or hooked) function on a loaded snapshot.
struct Request {
    std::uint32_t address = 0;                // entry address as the game calls it
    std::optional<std::uint32_t> gpr[8];      // a0-a3, t0-t3 (r4-r11); unset keeps the snapshot value
    std::optional<std::uint32_t> fpr[8];      // f12-f19
    std::optional<std::uint32_t> gp, sp;      // r28; r29 is the top below which the frame is carved
    std::uint32_t stack_reserve = 0x1000;     // bytes carved below sp: the frame, and the buffers above it
    std::int32_t budget = 500'000'000;        // EE cycles before the call counts as hung (1.7 s of EE time)
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> pokes;   // bytes stored before the call
    std::vector<Buffer> buffers;
    bool trace = false;                       // record function entries (hooks.cpp enter())
};

enum class Status { ok, no_code, bad_request, hung, thread_switch, forbidden_syscall, halted };

struct Range {
    std::uint32_t address, length;
    std::uint64_t hash;                       // FNV-1a 64 of the bytes after the call
};

struct IoWrite {
    std::uint32_t address;
    std::uint64_t value;
    std::uint8_t size;
};

struct Result {
    Status status = Status::ok;
    std::string detail;
    Cpu before{}, after{};                    // register files at entry and at the jr ra
    std::uint32_t sp = 0;                     // the sp the function was entered with
    std::uint32_t scratch = 0;                // first byte of the buffer area
    std::int64_t cycles = 0;                  // budget consumed by recompiled code (0 for a hook)
    std::vector<Range> ram, spr, stack;       // changed 16-byte-granular ranges; stack = RAM below sp
    std::vector<IoWrite> io;                  // stores that reached hardware registers
    std::vector<std::uint32_t> syscalls;      // syscall numbers, in order
    std::vector<std::uint32_t> entries;       // function entries seen when trace is set
    bool hook_ran = false;                    // the entry itself was served by a hook
};

Result call(const Request &request);
bool write(const Result &result, const std::string &json_path);   // also writes json_path + ".mem"
bool parse_option(const std::string &flag, const char *value, Request &request);
const char *status_name(Status status);

// Switches the rest of the runtime consults while a call runs.
extern bool g_active;
extern std::uint64_t g_time;                  // what now() reports during a call
bool preempt(Cpu &c);                         // what ps2::preempt() does during a call
// true: forbidden, c.pc already unwound. custom_handler: the game installed a SetSyscall handler for
// this number, which would run through kernel::run_guest() (its loop strips kUnwind and refills the
// budget, so an abort inside it could never reach call()); such calls are forbidden too.
bool syscall(Cpu &c, std::uint32_t number, std::uint32_t pc, bool custom_handler);
void io_write(std::uint32_t address, std::uint64_t value, unsigned size);
void entered(std::uint32_t address);          // from hooks.cpp enter(), when entries are traced
void hooked(std::uint32_t address);           // from hooks.cpp enter(), when a hook serves the entry

}
