#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void register_generated_code();

namespace {

bool parse_press(const std::string &spec, ps2::Press &press) {
    static const char *const kNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                           "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    const auto colon = spec.find(':');
    if (colon == std::string::npos) return false;
    press.frame = std::strtoull(spec.c_str(), nullptr, 10);
    std::string rest = spec.substr(colon + 1);
    press.frames = 6;
    const auto second = rest.find(':');
    if (second != std::string::npos) {
        press.frames = static_cast<std::uint32_t>(std::strtoul(rest.c_str() + second + 1, nullptr, 10));
        rest = rest.substr(0, second);
    }
    press.buttons = 0;
    std::size_t start = 0;
    while (start <= rest.size()) {
        const auto plus = rest.find('+', start);
        const std::string name = rest.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        int bit = -1;
        for (int i = 0; i < 16; ++i)
            if (name == kNames[i]) bit = i;
        if (bit < 0) return false;
        press.buttons |= static_cast<std::uint16_t>(1u << bit);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return true;
}

int usage() {
    std::fprintf(stderr,
                 "usage: ps2run <elf> [--arg text]... [--frames n] [--ntsc] [--misses file]\n"
                 "              [--disc image.iso] [--trace-syscalls] [--trace-io] [--trace-threads]\n"
                 "              [--trace-iop] [--trace-iop-hle] [--frames-dir dir] [--frame-every n]\n"
                 "              [--press frame:button[+button][:frames]]...\n");
    return 2;
}

}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    std::vector<std::string> args{argv[1]};
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ntsc") ps2::g_options.pal = false;
        else if (a == "--misses" && i + 1 < argc) ps2::g_options.misses_path = argv[++i];
        else if (a == "--trace-syscalls") ps2::g_options.trace_syscalls = true;
        else if (a == "--trace-io") ps2::g_options.trace_io = true;
        else if (a == "--trace-threads") ps2::g_options.trace_threads = true;
        else if (a == "--disc" && i + 1 < argc) ps2::g_options.disc_path = argv[++i];
        else if (a == "--trace-iop") ps2::g_options.trace_iop_modules = true;
        else if (a == "--trace-iop-hle") ps2::g_options.trace_iop_hle = true;
        else if (a == "--frames-dir" && i + 1 < argc) ps2::g_options.frames_dir = argv[++i];
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (!parse_press(argv[++i], press)) return usage();
            ps2::g_options.presses.push_back(press);
        }
        else if (a == "--frame-every" && i + 1 < argc) ps2::g_options.frame_every = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else return usage();
    }

    register_generated_code();
    std::uint32_t entry = 0;
    if (!ps2::load_elf(argv[1], entry)) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    ps2::boot(entry, args);
    return ps2::run();
}
