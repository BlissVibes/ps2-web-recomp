#include "ps2/disc.hpp"
#include "ps2/harness.hpp"
#include "ps2/hook.hpp"
#include "ps2/system.hpp"

#ifdef PS2_WINDOW
#include "window.hpp"
#endif
#ifdef PS2_PROFILE
#include "profile.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void register_generated_code();
void register_generated_vu();
void register_hooks();

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: ps2run <elf> [--arg text]... [--frames n] [--ntsc] [--misses file]\n"
                 "              [--disc image.iso [--disc-log file]] [--memcard card.ps2] [--pal | --ntsc] [--trace-syscalls] [--trace-io] [--trace-threads]\n"
                 "              [--trace-iop] [--trace-iop-hle] [--frames-dir dir] [--frame-every n]\n"
                 "              [--press frame:button[+button][:frames]]... [--window]\n"
                 "              [--save-state frame:file] [--load-state file]\n"
                 "              [--snapshot-every n --snapshot-dir dir]\n"
                 "              [--call addr --snapshot file [--a0..--a3 --t0..--t3 v|x64:hex] [--f12..--f19 v] [--gp v] [--sp v]\n"
                 "               [--fcr31 hex] [--stack-reserve n] [--buf reg:size[:hex]] [--poke addr:hex] [--budget n] [--trace-calls]\n"
                 "               [--hook name|addr] [--hooks all] [--out file.json]]\n"
                 "              [--shadow N]   with hooks: compare every Nth call of each hook with the recompiled function\n");
    return 2;
}

// --disc-log: the disc is read as --disc reads it, and every read is also written to `log_path` as a line
// "frame offset bytes". The page reads the same disc through its chunk cache (host/disc_cache.cpp); a log of
// a route lets that cache be tried with other sizes without a browser (scripts/disc_replay.cpp).
bool open_logged_disc(const std::string &path, const std::string &log_path) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    std::FILE *log = std::fopen(log_path.c_str(), "w");
    if (!file || !log) return false;
#if defined(_WIN32)
    _fseeki64(file, 0, SEEK_END);
    const std::uint64_t size = static_cast<std::uint64_t>(_ftelli64(file));
#else
    fseeko(file, 0, SEEK_END);
    const std::uint64_t size = static_cast<std::uint64_t>(ftello(file));
#endif
    return ps2::disc::open(path, size, [file, log](std::uint64_t offset, void *out, std::size_t bytes) {
        std::fprintf(log, "%llu %llu %zu\n", static_cast<unsigned long long>(ps2::frames_run()), static_cast<unsigned long long>(offset), bytes);
#if defined(_WIN32)
        _fseeki64(file, static_cast<long long>(offset), SEEK_SET);
#else
        fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
        return std::fread(out, 1, bytes, file);
    });
}

}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    std::vector<std::string> args{argv[1]};
    [[maybe_unused]] bool window = false;
    std::string load_path;
    ps2::harness::Request call;
    std::string call_out;
    std::vector<std::string> hooks;
    bool all_hooks = false;
    std::uint32_t shadow = 0;
    std::string disc_log;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ntsc" || a == "--pal") {
            ps2::g_options.pal = a == "--pal";
            ps2::g_options.rate_forced = true;
        }
        else if (a == "--misses" && i + 1 < argc) ps2::g_options.misses_path = argv[++i];
        else if (a == "--trace-syscalls") ps2::g_options.trace_syscalls = true;
        else if (a == "--trace-io") ps2::g_options.trace_io = true;
        else if (a == "--trace-threads") ps2::g_options.trace_threads = true;
        else if (a == "--disc" && i + 1 < argc) ps2::g_options.disc_path = argv[++i];
        else if (a == "--disc-log" && i + 1 < argc) disc_log = argv[++i];
        else if (a == "--memcard" && i + 1 < argc) ps2::g_options.memcard_path = argv[++i];
        else if (a == "--trace-iop") ps2::g_options.trace_iop_modules = true;
        else if (a == "--trace-iop-hle") ps2::g_options.trace_iop_hle = true;
        else if (a == "--frames-dir" && i + 1 < argc) ps2::g_options.frames_dir = argv[++i];
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (!ps2::parse_press(argv[++i], press)) return usage();
            ps2::g_options.presses.push_back(press);
        }
#ifdef PS2_WINDOW
        else if (a == "--window") window = true;
#endif
        else if (a == "--frame-every" && i + 1 < argc) ps2::g_options.frame_every = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--save-state" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) return usage();
            ps2::g_options.save_state_frame = std::strtoull(spec.substr(0, colon).c_str(), nullptr, 10);
            ps2::g_options.save_state_path = spec.substr(colon + 1);
        }
        else if (a == "--load-state" && i + 1 < argc) load_path = argv[++i];
        else if (a == "--snapshot-every" && i + 1 < argc) ps2::g_options.snapshot_every = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--snapshot-dir" && i + 1 < argc) ps2::g_options.snapshot_dir = argv[++i];
        else if (a == "--call" && i + 1 < argc) call.address = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 16));
        else if (a == "--snapshot" && i + 1 < argc) load_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) call_out = argv[++i];
        else if (a == "--trace-calls") call.trace = true;
        else if (a == "--hook" && i + 1 < argc) hooks.push_back(argv[++i]);
        else if (a == "--hooks" && i + 1 < argc && std::string(argv[i + 1]) == "all") { all_hooks = true; ++i; }
        else if (a == "--shadow" && i + 1 < argc) shadow = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (i + 1 < argc && ps2::harness::parse_option(a, argv[i + 1], call)) ++i;
        else return usage();
    }

#ifdef PS2_WINDOW
    if (window && !open_window()) return 1;
#endif
    register_generated_code();
    register_generated_vu();
    register_hooks();
    if (all_hooks) ps2::enable_all_hooks();
    ps2::set_hook_shadow(shadow);
    for (const std::string &name : hooks) {
        if (!ps2::enable_hook(name)) {
            std::fprintf(stderr, "unknown hook %s\n", name.c_str());
            return 2;
        }
    }
    if (ps2::g_options.snapshot_every && ps2::g_options.snapshot_dir.empty()) {
        std::fprintf(stderr, "--snapshot-every needs --snapshot-dir\n");
        return 2;
    }
    if (call.address && load_path.empty()) {
        std::fprintf(stderr, "--call needs --snapshot\n");
        return 2;
    }
    if (!disc_log.empty()) {
        if (ps2::g_options.disc_path.empty() || !open_logged_disc(ps2::g_options.disc_path, disc_log)) {
            std::fprintf(stderr, "--disc-log needs a --disc that opens and a log file that can be written\n");
            return 2;
        }
        ps2::g_options.disc_path.clear();   // open already: boot must not open it again without the log
    }
    std::uint32_t entry = 0;
    if (!ps2::load_elf(argv[1], entry)) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    ps2::boot(entry, args);
    if (!load_path.empty() && !ps2::load_state(load_path)) return 1;
    if (call.address) {
        if (call.trace) ps2::set_entry_trace(true);
        const ps2::harness::Result result = ps2::harness::call(call);
        std::fprintf(stderr, "[call] 0x%08X %s%s%s v0 %016llX v1 %016llX f0 %08X cycles %lld ram %zu spr %zu stack %zu io %zu syscalls %zu%s\n",
                     call.address, ps2::harness::status_name(result.status), result.detail.empty() ? "" : ": ", result.detail.c_str(),
                     static_cast<unsigned long long>(result.after.r[2].d[0]), static_cast<unsigned long long>(result.after.r[3].d[0]), result.after.f[0],
                     static_cast<long long>(result.cycles), result.ram.size(), result.spr.size(), result.stack.size(), result.io.size(), result.syscalls.size(),
                     result.hook_ran ? " (hook)" : "");
        if (!call_out.empty() && !ps2::harness::write(result, call_out)) {
            std::fprintf(stderr, "cannot write %s\n", call_out.c_str());
            return 1;
        }
        return result.status == ps2::harness::Status::ok ? 0 : 3;
    }
#ifdef PS2_PROFILE
    const char *profile = std::getenv("PS2_PROFILE");
    const char *profile_from = std::getenv("PS2_PROFILE_FROM");
    if (profile) start_profile(profile_from ? std::strtoull(profile_from, nullptr, 10) : 0);
    const int result = ps2::run();
    if (profile) write_profile(profile);
#else
    const int result = ps2::run();
#endif
    ps2::report_hooks();
    return result;
}
