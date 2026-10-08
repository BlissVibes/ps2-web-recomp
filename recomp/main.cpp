#include "analysis.hpp"
#include "codegen.hpp"
#include "elf.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace recomp;

namespace {

std::vector<std::uint32_t> read_entries(const std::string &path) {
    std::vector<std::uint32_t> entries;
    std::ifstream file(path);
    std::string token;
    while (file >> token) {
        if (token[0] == '#') { std::getline(file, token); continue; }
        entries.push_back(static_cast<std::uint32_t>(std::stoul(token, nullptr, 16)));
    }
    return entries;
}

int usage() {
    std::fprintf(stderr, "usage: eerecomp <elf> [--entries file] [--out dir] [--stats]\n");
    return 2;
}

}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    std::string elf_path = argv[1], entries_path, out;
    bool stats = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--entries" && i + 1 < argc) entries_path = argv[++i];
        else if (arg == "--stats") stats = true;
        else if (arg == "--out" && i + 1 < argc) out = argv[++i];
        else return usage();
    }

    try {
        const Image image = Image::load(elf_path);
        Program program = analyse(image, entries_path.empty() ? std::vector<std::uint32_t>{} : read_entries(entries_path));
        std::size_t code = 0;
        std::map<Op, std::size_t> histogram;
        for (std::uint32_t pc = program.base; pc < program.end; pc += 4) {
            if (!program.is_code(pc)) continue;
            ++code;
            ++histogram[decode(*image.word(pc), pc).op];
        }
        std::printf("code %zu instructions (%zu KiB), functions %zu, entries %zu, rejected seeds %zu\n",
                    code, code * 4 / 1024, program.functions.size(), program.entries.size(), program.rejected.size());
        if (stats)
            for (const auto &[op, count] : histogram) std::printf("%10zu %s\n", count, name(op));
        if (!out.empty()) generate(image, program, CodegenOptions{out});
    } catch (const std::exception &error) {
        std::fprintf(stderr, "eerecomp: %s\n", error.what());
        return 1;
    }
    return 0;
}
