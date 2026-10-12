#include "ps2/hook.hpp"

#include "kernel.hpp"
#include "ps2/generated.hpp"
#include "ps2/harness.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace ps2 {

bool g_hooks = false;

namespace {

struct Entry {
    const Hook *hook;
    bool enabled;
};

std::unordered_map<std::uint32_t, Entry> g_table;
bool g_trace = false;
std::uint32_t g_snap_address = 0, g_snap_every = 1, g_snap_max = 0, g_snap_seen = 0, g_snap_saved = 0;
std::string g_snap_dir;

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

void set_entry_snapshot(std::uint32_t address, std::uint32_t every, std::uint32_t max, const std::string &dir) {
    g_snap_address = address;
    g_snap_every = every ? every : 1;
    g_snap_max = max;
    g_snap_dir = dir;
    refresh();
}

// Called at every function entry dispatch() reaches while g_hooks is set.
// Returns true when a hook served the call: the caller must return at once.
bool enter(Cpu &c, std::uint32_t address) {
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
    it->second.hook->fn(c);
    c.pc = c.r[31].w[0];
    return true;
}

}

#ifndef PS2_HOOKS
void register_hooks() {}
#endif
