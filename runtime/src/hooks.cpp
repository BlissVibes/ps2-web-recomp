#include "ps2/hook.hpp"

#include "kernel.hpp"
#include "ps2/generated.hpp"
#include "ps2/harness.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2 {

bool g_hooks = false;

namespace {

struct Entry {
    const Hook *hook;
    bool enabled;
    std::uint64_t calls = 0, compared = 0, wrong = 0, v0 = 0, f0 = 0, fcr31 = 0, scratch = 0, skipped = 0;
};

std::unordered_map<std::uint32_t, Entry> g_table;
bool g_trace = false;
std::uint32_t g_snap_address = 0, g_snap_every = 1, g_snap_max = 0, g_snap_seen = 0, g_snap_saved = 0;
std::string g_snap_dir;
std::uint32_t g_offered = 0;   // the entry dispatch() has just offered and no hook took
std::uint32_t g_shadow = 0;    // shadow mode: compare every this many calls of a hook; 0 = off
bool g_shadowing = false;      // inside a comparison: the recompiled function runs untouched

bool take(Cpu &c, std::uint32_t address);

void refresh() {
    g_hooks = g_trace || g_snap_address != 0 ||
              std::any_of(g_table.begin(), g_table.end(), [](const auto &kv) { return kv.second.enabled; });
}

}

void register_hooks(const Hook *hooks, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) g_table[hooks[i].address] = {&hooks[i], false};
}

bool enable_hook(const std::string &key) {
    bool found = false;
    const bool hex = key.rfind("0x", 0) == 0 || key.rfind("0X", 0) == 0;
    for (auto &[address, e] : g_table) {
        if (key == e.hook->name || (hex && std::strtoul(key.c_str(), nullptr, 16) == address)) e.enabled = found = true;
    }
    refresh();
    return found;
}

void enable_all_hooks() {
    for (auto &[address, e] : g_table) e.enabled = true;
    refresh();
}

void set_entry_trace(bool on) { g_trace = on; refresh(); }

void set_hook_shadow(std::uint32_t every) { g_shadow = every; }

void set_entry_snapshot(std::uint32_t address, std::uint32_t every, std::uint32_t max, const std::string &dir) {
    g_snap_address = address;
    g_snap_every = every ? every : 1;
    g_snap_max = max;
    g_snap_dir = dir;
    refresh();
}

bool enter(Cpu &c, std::uint32_t address) {
    const bool offered = address == g_offered;   // dispatch() has just offered this very entry
    g_offered = 0;
    return !offered && take(c, address);
}

bool offer(Cpu &c, std::uint32_t address) {
    const bool taken = take(c, address);
    g_offered = taken ? 0 : address;
    return taken;
}

void report_hooks() {
    std::vector<const Entry *> used;
    for (const auto &[address, e] : g_table)
        if (e.enabled) used.push_back(&e);
    std::sort(used.begin(), used.end(), [](const Entry *a, const Entry *b) { return a->hook->address < b->hook->address; });
    for (const Entry *e : used) {
        if (!g_shadow) {
            std::fprintf(stderr, "[hook] 0x%08X %s: %llu calls\n", e->hook->address, e->hook->name, static_cast<unsigned long long>(e->calls));
            continue;
        }
        std::fprintf(stderr, "[hook] 0x%08X %s: %llu calls, %llu compared: %llu wrong (kept registers or memory), v0 differs in %llu, f0 in %llu, fcr31 in %llu, other scratch registers in %llu; %llu not compared\n",
                     e->hook->address, e->hook->name, static_cast<unsigned long long>(e->calls), static_cast<unsigned long long>(e->compared),
                     static_cast<unsigned long long>(e->wrong), static_cast<unsigned long long>(e->v0), static_cast<unsigned long long>(e->f0),
                     static_cast<unsigned long long>(e->fcr31), static_cast<unsigned long long>(e->scratch), static_cast<unsigned long long>(e->skipped));
    }
}

namespace {

// Shadow mode (set_hook_shadow, --shadow N): an enabled hook does not serve its calls. On every Nth call
// the recompiled function runs on the live call, then the hook runs on a copy of the same call, and the
// two outcomes are compared; the recompiled outcome stays. So the game runs exactly as it does without
// hooks, cycle for cycle, and each hook is checked on the calls the game really makes. A call during
// which the runtime stopped to run events is not compared (memory may have changed under it).
constexpr std::uint32_t kFrame = 0x4000;       // memory this far below the caller's sp is the callee's own frame: not compared
constexpr int kShown = 4;                      // differences printed per hook and kind

const char *const kRegisters[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

void differ(std::string &list, const char *name, int index, std::uint64_t recompiled, std::uint64_t hooked) {
    if (recompiled == hooked) return;
    char text[96];
    if (index >= 0) std::snprintf(text, sizeof text, " %s%d %llX/%llX", name, index, static_cast<unsigned long long>(recompiled), static_cast<unsigned long long>(hooked));
    else std::snprintf(text, sizeof text, " %s %llX/%llX", name, static_cast<unsigned long long>(recompiled), static_cast<unsigned long long>(hooked));
    list += text;
}

// The first address where two copies of memory differ, outside [skip, skip + skipped); size if none.
std::uint32_t first_difference(const std::uint8_t *a, const std::uint8_t *b, std::uint32_t size, std::uint32_t skip, std::uint32_t skipped) {
    for (std::uint32_t at = 0; at < size;) {
        if (at >= skip && at < skip + skipped) {
            at = skip + skipped;
            continue;
        }
        const std::uint32_t run = std::min<std::uint32_t>(std::min<std::uint32_t>(4096, size - at), at < skip ? skip - at : size - at);
        if (std::memcmp(a + at, b + at, run) != 0)
            for (std::uint32_t i = at; i < at + run; ++i)
                if (a[i] != b[i]) return i;
        at += run;
    }
    return size;
}

// Compares the hook's outcome (c and memory as they are) with the recompiled function's (rec, ram, spr).
// Wrong: the registers a function must keep, and memory outside the callee's frame. v0 and f0 are
// counted on their own: one of them is the function's result and the other a register it may use
// freely, and only the function's description says which. Scratch: every other register.
// Two exceptions to "scratch". The FPU status (fcr31) is counted on its own, the condition bit apart from
// the flags: a compare leaves its result there and a divide its sticky flags, some hooks reproduce both on
// purpose, and among the scratch registers, where nearly every call differs, a miss would not be seen.
// The upper halves of hi and lo and the shift amount count as wrong: a hook only has them as the function
// leaves them if it sets them itself, and no other check would show that it does not.
void compare(Entry &e, const Cpu &before, const Cpu &rec, const Cpu &c, const std::uint8_t *ram, const std::uint8_t *spr) {
    std::string wrong, scratch, status;
    for (int i = 1; i < 32; ++i) {
        const bool kept = (i >= 16 && i <= 23) || i >= 28;
        differ(kept ? wrong : scratch, kRegisters[i], -1, i == 2 ? 0 : rec.r[i].d[0], i == 2 ? 0 : c.r[i].d[0]);
        const std::string upper = std::string(kRegisters[i]) + ".upper";
        differ(kept ? wrong : scratch, upper.c_str(), -1, rec.r[i].d[1], c.r[i].d[1]);
    }
    for (int i = 1; i < 32; ++i) differ(i >= 20 ? wrong : scratch, "f", i, rec.f[i], c.f[i]);
    differ(status, "condition", -1, (rec.fcr31 & kFpuCondition) != 0, (c.fcr31 & kFpuCondition) != 0);
    differ(status, "flags", -1, rec.fcr31 & ~kFpuCondition, c.fcr31 & ~kFpuCondition);
    differ(scratch, "facc", -1, rec.facc, c.facc);
    differ(scratch, "hi", -1, rec.hi.d[0], c.hi.d[0]);
    differ(scratch, "lo", -1, rec.lo.d[0], c.lo.d[0]);
    differ(wrong, "hi1", -1, rec.hi.d[1], c.hi.d[1]);
    differ(wrong, "lo1", -1, rec.lo.d[1], c.lo.d[1]);
    differ(wrong, "sa", -1, rec.sa, c.sa);
    const std::uint32_t sp = before.r[29].w[0];
    const std::uint32_t frame = sp >= kFrame && sp <= kRamSize ? sp - kFrame : kRamSize;
    const std::uint32_t at = first_difference(ram, g_ram, kRamSize, frame, kFrame);
    if (at < kRamSize) {
        char text[96];
        std::snprintf(text, sizeof text, " memory at 0x%08X %02X/%02X", at, ram[at], g_ram[at]);
        wrong += text;
    }
    const std::uint32_t pad = first_difference(spr, g_spr, kScratchpadSize, kScratchpadSize, 0);
    if (pad < kScratchpadSize) {
        char text[96];
        std::snprintf(text, sizeof text, " scratchpad at 0x%04X %02X/%02X", pad, spr[pad], g_spr[pad]);
        wrong += text;
    }
    ++e.compared;
    if (rec.r[2].d[0] != c.r[2].d[0]) ++e.v0;
    if (rec.f[0] != c.f[0]) ++e.f0;
    const auto show = [&](const char *kind, const std::string &list, std::uint64_t &count) {
        if (list.empty() || ++count > kShown) return;
        std::fprintf(stderr, "[shadow] 0x%08X %s call %llu %s (recompiled/hook):%s; a0 %08X a1 %08X a2 %08X a3 %08X f12 %08X f13 %08X ra %08X\n", e.hook->address,
                     e.hook->name, static_cast<unsigned long long>(e.calls), kind, list.c_str(), before.r[4].w[0], before.r[5].w[0], before.r[6].w[0],
                     before.r[7].w[0], before.f[12], before.f[13], before.r[31].w[0]);
    };
    show("wrong", wrong, e.wrong);
    show("fcr31", status, e.fcr31);
    show("scratch", scratch, e.scratch);
}

bool shadow(Cpu &c, Entry &e, std::uint32_t address) {
    static std::vector<std::uint8_t> ram[2] = {std::vector<std::uint8_t>(kRamSize), std::vector<std::uint8_t>(kRamSize)};
    static std::vector<std::uint8_t> spr[2] = {std::vector<std::uint8_t>(kScratchpadSize), std::vector<std::uint8_t>(kScratchpadSize)};
    const auto keep = [&](int k) {
        std::memcpy(ram[k].data(), g_ram, kRamSize);
        std::memcpy(spr[k].data(), g_spr, kScratchpadSize);
    };
    const auto restore = [&](int k) {
        std::memcpy(g_ram, ram[k].data(), kRamSize);
        std::memcpy(g_spr, spr[k].data(), kScratchpadSize);
    };
    const Cpu before = c;
    const std::uint32_t ra = c.r[31].w[0];
    keep(0);
    const std::uint64_t preempts = preempt_count();
    g_shadowing = true;
    call(c, address, 0);
    g_shadowing = false;
    if (preempt_count() != preempts || c.pc != ra) {
        // Events ran inside the call, or it did not come back to its caller (an unwind, a halt). The state
        // is what it would be without hooks; there is nothing sound to compare it with.
        ++e.skipped;
        return true;
    }
    const Cpu rec = c;
    keep(1);
    restore(0);
    c = before;
    e.hook->fn(c);
    c.pc = ra;
    compare(e, before, rec, c, ram[1].data(), spr[1].data());
    // PS2_SHADOW_LIVE: which outcome stays. Unset: the recompiled one. "hook": the hook's memory and registers, with
    // the register groups also named (gpr, fpr, hilo, fcr) taken from the recompiled outcome. The cycles used are
    // the recompiled function's in every case, so such a run differs from one without hooks in nothing but the outcome.
    static const std::string live = [] {
        const char *text = std::getenv("PS2_SHADOW_LIVE");
        return std::string(text ? text : "");
    }();
    if (live.find("hook") == std::string::npos) {
        restore(1);
        c = rec;
    } else {
        if (live.find("gpr") != std::string::npos)
            for (int i = 1; i < 32; ++i) {
                const bool kept = (i >= 16 && i <= 23) || i >= 28;
                if (!kept && i != 2) c.r[i] = rec.r[i];
                if (i == 2) c.r[i].d[1] = rec.r[i].d[1];
            }
        if (live.find("fpr") != std::string::npos)
            for (int i = 0; i < 20; ++i) c.f[i] = rec.f[i];
        if (live.find("hilo") != std::string::npos) {
            c.hi = rec.hi;
            c.lo = rec.lo;
        }
        if (live.find("fcr") != std::string::npos) {
            c.fcr31 = rec.fcr31;
            c.facc = rec.facc;
        }
    }
    c.budget = rec.budget;
    return true;
}

// One entry of a function while g_hooks is set: the trace, the entry snapshot, and its hook if one is enabled.
bool take(Cpu &c, std::uint32_t address) {
    if (g_shadowing) return false;
    if (g_trace) harness::entered(address);
    if (g_snap_address == address && !harness::g_active && !kernel::in_handler() && (g_snap_max == 0 || g_snap_saved < g_snap_max) && g_snap_seen++ % g_snap_every == 0) {
        char name[96];
        std::snprintf(name, sizeof name, "/e%08X_f%06llu_%04u.entry.state", address, static_cast<unsigned long long>(frame_count()), g_snap_saved);
        c.pc = address;   // the generated code does not keep pc current between control transfers
        if (save_state(g_snap_dir + name)) ++g_snap_saved;
    }
    const auto it = g_table.find(address);
    if (it == g_table.end() || !it->second.enabled) return false;
    harness::hooked(address);
    ++it->second.calls;
    if (g_shadow && !harness::g_active) return it->second.calls % g_shadow == 0 && shadow(c, it->second, address);
    it->second.hook->fn(c);
    c.pc = c.r[31].w[0];
    return true;
}

}

}

#ifndef PS2_HOOKS
void register_hooks() {}
#endif
