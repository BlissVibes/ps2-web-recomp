#pragma once

#include "analysis.hpp"
#include "elf.hpp"

#include <cstdint>
#include <string>

namespace recomp {

struct CodegenOptions {
    std::string output;
    std::uint32_t unit_bits = 14;
};

void generate(const Image &image, Program &program, const CodegenOptions &options);

}
