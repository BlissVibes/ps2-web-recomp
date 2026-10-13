#include "ps2/harness.hpp"

#include "kernel.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace ps2::harness {

bool g_active = false;
std::uint64_t g_time = 0;

namespace {

constexpr std::uint32_t kFallbackStackTop = 0x00080000u;   // the kernel stack run_guest uses
constexpr std::uint32_t kStackWindow = 0x10000u;            // writes this far below sp count as the frame

Status g_abort = Status::ok;
std::string g_abort_detail;
Result *g_result = nullptr;

// Syscalls that end, create, start or block threads, or change the kernel. Everything else runs and is
// recorded. A SignalSema that wakes a higher-priority thread still switches threads through
// reschedule(); call() catches that afterwards through kUnwind and the current-thread check.
bool forbidden(std::uint32_t n) {
    switch (n) {
    case 0x04: case 0x06: case 0x07:                                              // Exit, LoadExecPS2, ExecPS2
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:  // thread create .. iTerminateThread
    case 0x32: case 0x44:                                                         // SleepThread, WaitSema
    case 0x3C: case 0x3D: case 0x74:                                              // SetupThread, SetupHeap, SetSyscall
        return true;
    default:
        return false;
    }
}

std::uint64_t fnv1a(const std::uint8_t *p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

void diff(const std::uint8_t *before, const std::uint8_t *after, std::uint32_t size, std::uint32_t base, std::vector<Range> &out) {
    constexpr std::uint32_t kBlock = 16;
    std::uint32_t start = UINT32_MAX;
    for (std::uint32_t at = 0; at <= size; at += kBlock) {
        const bool differs = at < size && std::memcmp(before + at, after + at, kBlock) != 0;
        if (differs && start == UINT32_MAX) start = at;
        if (!differs && start != UINT32_MAX) {
            out.push_back({base + start, at - start, fnv1a(after + start, at - start)});
            start = UINT32_MAX;
        }
    }
}

std::uint32_t parse_u32(const char *text) {
    // 0x.. hex, decimal, or f:1.5 for the bit pattern of a float
    if (text[0] == 'f' && text[1] == ':') {
        const float f = std::strtof(text + 2, nullptr);
        std::uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return bits;
    }
    return static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0));
}

std::vector<std::uint8_t> parse_hex(const std::string &text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
    return out;
}

int reg_index(const std::string &name) {
    static const char *const kNames[8] = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"};
    for (int i = 0; i < 8; ++i)
        if (name == kNames[i]) return i;
    return -1;
}

void hex128(std::FILE *f, const Reg128 &r) {
    std::fprintf(f, "\"%016llx%016llx\"", static_cast<unsigned long long>(r.d[1]), static_cast<unsigned long long>(r.d[0]));
}

void cpu_json(std::FILE *f, const Cpu &c) {
    std::fprintf(f, "{\"r\":[");
    for (int i = 0; i < 32; ++i) { if (i) std::fputc(',', f); hex128(f, c.r[i]); }
    std::fprintf(f, "],\"hi\":"); hex128(f, c.hi);
    std::fprintf(f, ",\"lo\":"); hex128(f, c.lo);
    std::fprintf(f, ",\"sa\":%u,\"pc\":\"0x%08X\",\"f\":[", c.sa, c.pc);
    for (int i = 0; i < 32; ++i) std::fprintf(f, "%s\"0x%08X\"", i ? "," : "", c.f[i]);
    std::fprintf(f, "],\"facc\":\"0x%08X\",\"fcr31\":\"0x%08X\",\"vf\":[", c.facc, c.fcr31);
    for (int i = 0; i < 32; ++i) { if (i) std::fputc(',', f); hex128(f, c.vu0.vf[i]); }
    std::fprintf(f, "],\"acc\":"); hex128(f, c.vu0.acc);
    std::fprintf(f, ",\"vi\":[");
    for (int i = 0; i < 16; ++i) std::fprintf(f, "%s%u", i ? "," : "", c.vu0.vi[i]);
    std::fprintf(f, "],\"q\":\"0x%08X\",\"p\":\"0x%08X\",\"i\":\"0x%08X\",\"vr\":\"0x%08X\",\"mac\":\"0x%08X\",\"status\":\"0x%08X\",\"clip\":\"0x%08X\"}",
                 c.vu0.q, c.vu0.p, c.vu0.i, c.vu0.r, c.vu0.mac, c.vu0.status, c.vu0.clip);
}

void ranges_json(std::FILE *f, const char *name, const std::vector<Range> &v) {
    std::fprintf(f, "\"%s\":[", name);
    for (std::size_t i = 0; i < v.size(); ++i)
        std::fprintf(f, "%s{\"at\":\"0x%08X\",\"len\":%u,\"fnv\":\"%016llx\"}", i ? "," : "", v[i].address, v[i].length, static_cast<unsigned long long>(v[i].hash));
    std::fputc(']', f);
}

void ranges_bytes(std::FILE *m, std::uint8_t kind, const std::vector<Range> &v, const std::uint8_t *memory, std::uint32_t base) {
    for (const Range &r : v) {
        std::fwrite(&kind, 1, 1, m);
        std::fwrite(&r.address, 4, 1, m);
        std::fwrite(&r.length, 4, 1, m);
        std::fwrite(memory + (r.address - base), 1, r.length, m);
    }
}

}

const char *status_name(Status s) {
    switch (s) {
    case Status::ok: return "ok";
    case Status::no_code: return "no_code";
    case Status::bad_request: return "bad_request";
    case Status::hung: return "hung";
    case Status::thread_switch: return "thread_switch";
    case Status::forbidden_syscall: return "forbidden_syscall";
    case Status::halted: return "halted";
    }
    return "?";
}

bool preempt(Cpu &) {
    // Never run events, the IOP or the scheduler inside a call. The budget ran out: the call hangs.
    g_abort = Status::hung;
    g_abort_detail = "cycle budget exhausted";
    return true;   // the generated code sets c.pc |= kUnwind and returns to call()
}

bool syscall(Cpu &c, std::uint32_t number, std::uint32_t pc, bool custom_handler) {
    if (g_result) g_result->syscalls.push_back(number);
    if (!forbidden(number) && !custom_handler) return false;
    g_abort = Status::forbidden_syscall;
    char text[96];
    std::snprintf(text, sizeof text, "%ssyscall 0x%02X at 0x%08X", custom_handler ? "custom-handler " : "", number, pc);
    g_abort_detail = text;
    c.pc = pc | kUnwind;
    return true;
}

void io_write(std::uint32_t address, std::uint64_t value, unsigned size) {
    if (g_result) g_result->io.push_back({address, value, static_cast<std::uint8_t>(size)});
}

void entered(std::uint32_t address) {
    if (g_result) g_result->entries.push_back(address);
}

void hooked(std::uint32_t address) {
    // Only the entry the harness asked for counts: a hooked callee leaves hook_ran alone.
    if (g_result && address == g_result->before.pc && g_cpu.r[31].w[0] == kHandlerReturn) g_result->hook_ran = true;
}

Result call(const Request &q) {
    Result out;
    Cpu &c = g_cpu;
    c.pc &= ~kUnwind;
    if (!has_code(q.address)) {
        out.status = Status::no_code;
        out.detail = "no translated entry at that address";
        return out;
    }

    // Arguments and the frame. The top is the snapshot thread's sp (or the given one); the frame is
    // carved below it and the harness buffers live just above the new sp, so writes into them count
    // as results while the function's own frame does not.
    for (int i = 0; i < 8; ++i) {
        if (q.gpr[i]) c.r[4 + i].sd[0] = static_cast<std::int32_t>(*q.gpr[i]);
        if (q.gpr64[i]) c.r[4 + i].d[0] = *q.gpr64[i];
        if (q.fpr[i]) c.f[12 + i] = *q.fpr[i];
    }
    if (q.gp) c.r[28].sd[0] = static_cast<std::int32_t>(*q.gp);
    if (q.fcr31) c.fcr31 = *q.fcr31;
    std::uint32_t top = q.sp ? *q.sp : c.r[29].w[0];
    if (!q.sp && (kernel::current_thread() == 0 || top < q.stack_reserve + 0x1000u || top > kRamSize)) top = kFallbackStackTop;
    if (top > kRamSize || top < q.stack_reserve + 0x1000u) {
        out.status = Status::bad_request;
        out.detail = "the stack top is outside EE RAM";
        return out;
    }
    const std::uint32_t sp = (top - q.stack_reserve) & ~15u;
    std::uint32_t scratch = sp + 16u;
    for (const Buffer &b : q.buffers) {
        const std::uint32_t size = (b.size + 15u) & ~15u;
        if (b.reg < 0 || b.reg > 7 || size > top || scratch + size > top) {
            out.status = Status::bad_request;
            out.detail = "buffers do not fit in the stack reserve";
            return out;
        }
        std::memset(g_ram + scratch, 0, size);
        std::memcpy(g_ram + scratch, b.bytes.data(), std::min<std::size_t>(b.bytes.size(), size));
        c.r[4 + b.reg].sd[0] = static_cast<std::int32_t>(scratch);
        scratch += size;
    }
    for (const auto &[address, bytes] : q.pokes)
        for (std::size_t i = 0; i < bytes.size(); ++i) st8(address + static_cast<std::uint32_t>(i), bytes[i]);
    c.r[29].sd[0] = static_cast<std::int32_t>(sp);
    c.r[31].sd[0] = static_cast<std::int32_t>(kHandlerReturn);
    c.pc = q.address;
    out.before = c;
    out.sp = sp;
    out.scratch = sp + 16u;

    std::vector<std::uint8_t> ram(g_ram, g_ram + kRamSize), spr(g_spr, g_spr + kScratchpadSize);
    const int thread = kernel::current_thread();
    g_result = &out;
    g_abort = Status::ok;
    g_abort_detail.clear();
    g_time = now();
    g_active = true;
    set_budget(c, q.budget);
    const std::int64_t given = c.budget;
    for (;;) {
        if (c.pc == kHandlerReturn) break;
        if ((c.pc & ~kUnwind) == kIdle) {
            // switch_to() found no runnable thread and parked the EE: the call did not return.
            out.status = Status::thread_switch;
            out.detail = "the kernel idled inside the call (no runnable thread)";
            break;
        }
        if (c.pc & kUnwind) {
            if (g_abort != Status::ok) {
                out.status = g_abort;
                out.detail = g_abort_detail;
            } else if (halted()) {
                out.status = Status::halted;
                out.detail = "runtime halted (see the [ee] lines above)";
            } else {
                out.status = Status::thread_switch;
                out.detail = kernel::current_thread() != thread ? "the kernel switched threads inside the call" : "unexpected unwind";
            }
            break;
        }
        if (halted()) {
            out.status = Status::halted;
            out.detail = "runtime halted (see the [ee] lines above)";
            break;
        }
        dispatch(c, 0, 0);
    }
    g_active = false;
    g_result = nullptr;
    out.cycles = given - c.budget;
    out.after = c;

    diff(ram.data(), g_ram, kRamSize, 0, out.ram);
    diff(spr.data(), g_spr, kScratchpadSize, kScratchpadBase, out.spr);
    const std::uint32_t low = sp >= kStackWindow ? sp - kStackWindow : 0;
    const auto frame = [&](const Range &r) { return r.address < sp && r.address >= low; };
    std::copy_if(out.ram.begin(), out.ram.end(), std::back_inserter(out.stack), frame);
    out.ram.erase(std::remove_if(out.ram.begin(), out.ram.end(), frame), out.ram.end());
    return out;
}

bool write(const Result &r, const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "{\"harness\":1,\"state_version\":%u,\"frame\":%llu,\"status\":\"%s\",\"detail\":\"%s\",\"hook_ran\":%s,\"sp\":\"0x%08X\",\"scratch\":\"0x%08X\",\"cycles\":%lld,\n",
                 state_version(), static_cast<unsigned long long>(frame_count()), status_name(r.status), r.detail.c_str(), r.hook_ran ? "true" : "false",
                 r.sp, r.scratch, static_cast<long long>(r.cycles));
    std::fprintf(f, "\"before\":"); cpu_json(f, r.before);
    std::fprintf(f, ",\n\"after\":"); cpu_json(f, r.after);
    std::fprintf(f, ",\n"); ranges_json(f, "ram", r.ram);
    std::fprintf(f, ",\n"); ranges_json(f, "spr", r.spr);
    std::fprintf(f, ",\n"); ranges_json(f, "stack", r.stack);
    std::fprintf(f, ",\n\"io\":[");
    for (std::size_t i = 0; i < r.io.size(); ++i)
        std::fprintf(f, "%s{\"at\":\"0x%08X\",\"size\":%u,\"value\":\"0x%016llx\"}", i ? "," : "", r.io[i].address, r.io[i].size, static_cast<unsigned long long>(r.io[i].value));
    std::fprintf(f, "],\n\"syscalls\":[");
    for (std::size_t i = 0; i < r.syscalls.size(); ++i) std::fprintf(f, "%s%u", i ? "," : "", r.syscalls[i]);
    std::fprintf(f, "],\n\"entries\":[");
    for (std::size_t i = 0; i < r.entries.size(); ++i) std::fprintf(f, "%s\"0x%08X\"", i ? "," : "", r.entries[i]);
    std::fprintf(f, "]}\n");
    std::fclose(f);

    // Sidecar: records of kind (1 ram, 2 spr, 3 stack), u32 address, u32 length, bytes as they are now.
    std::FILE *m = std::fopen((path + ".mem").c_str(), "wb");
    if (!m) return false;
    ranges_bytes(m, 1, r.ram, g_ram, 0);
    ranges_bytes(m, 2, r.spr, g_spr, kScratchpadBase);
    ranges_bytes(m, 3, r.stack, g_ram, 0);
    std::fclose(m);
    return true;
}

bool parse_option(const std::string &flag, const char *value, Request &q) {
    static const char *const kGpr[8] = {"--a0", "--a1", "--a2", "--a3", "--t0", "--t1", "--t2", "--t3"};
    for (int i = 0; i < 8; ++i) {
        if (flag == kGpr[i]) {
            // x64:HEX gives all 64 bits of the register; anything else is 32 bits, sign-extended.
            if (std::strncmp(value, "x64:", 4) == 0) q.gpr64[i] = std::strtoull(value + 4, nullptr, 16);
            else q.gpr[i] = parse_u32(value);
            return true;
        }
        if (flag == "--f" + std::to_string(12 + i)) { q.fpr[i] = parse_u32(value); return true; }
    }
    if (flag == "--gp") { q.gp = parse_u32(value); return true; }
    if (flag == "--sp") { q.sp = parse_u32(value); return true; }
    if (flag == "--fcr31") { q.fcr31 = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 16)); return true; }
    if (flag == "--stack-reserve") { q.stack_reserve = parse_u32(value); return true; }
    if (flag == "--budget") { q.budget = static_cast<std::int32_t>(std::strtol(value, nullptr, 0)); return true; }
    const std::string text = value;
    if (flag == "--poke") {   // --poke ADDR:HEXBYTES
        const auto colon = text.find(':');
        if (colon == std::string::npos) return false;
        q.pokes.emplace_back(parse_u32(text.substr(0, colon).c_str()), parse_hex(text.substr(colon + 1)));
        return true;
    }
    if (flag == "--buf") {    // --buf REG:SIZE[:HEXBYTES]
        const auto first = text.find(':');
        if (first == std::string::npos) return false;
        const auto second = text.find(':', first + 1);
        Buffer b;
        b.reg = reg_index(text.substr(0, first));
        b.size = parse_u32(text.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1).c_str());
        if (second != std::string::npos) b.bytes = parse_hex(text.substr(second + 1));
        if (b.reg < 0 || !b.size) return false;
        q.buffers.push_back(std::move(b));
        return true;
    }
    return false;
}

}
