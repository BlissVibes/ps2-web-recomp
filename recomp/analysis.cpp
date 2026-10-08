#include "analysis.hpp"

#include <algorithm>
#include <deque>

namespace recomp {

namespace {

constexpr std::size_t kTentativeLimit = 16384;

class Explorer {
public:
    Explorer(const Image &image, Program &program) : image_(image), p_(program) {}

    bool valid_target(std::uint32_t address) const {
        return (address & 3u) == 0 && address >= p_.base && address < p_.end && image_.executable(address);
    }

    void strong(std::uint32_t address, bool function) {
        if (!valid_target(address)) return;
        if (function) p_.functions.insert(address);
        p_.entries.insert(address);
        if (!p_.is_code(address)) queue_.push_back(address);
    }

    void drain() {
        while (!queue_.empty()) {
            const std::uint32_t start = queue_.front();
            queue_.pop_front();
            walk(start);
        }
    }

    bool tentative(std::uint32_t start) {
        std::deque<std::uint32_t> pending{start};
        std::set<std::uint32_t> seen;
        while (!pending.empty()) {
            std::uint32_t pc = pending.front();
            pending.pop_front();
            while (true) {
                if (!valid_target(pc)) return false;
                if (p_.is_code(pc) || seen.count(pc)) break;
                if (seen.size() > kTentativeLimit) return false;
                seen.insert(pc);
                const Insn insn = decode(*image_.word(pc), pc);
                if (insn.op == Op::Invalid) return false;
                const Flow f = flow(insn.op);
                if (f == Flow::Stop) break;
                if (f == Flow::None) { pc += 4; continue; }
                if (!valid_target(pc + 4) || decode(*image_.word(pc + 4), pc + 4).op == Op::Invalid) return false;
                if (has_delay_slot(decode(*image_.word(pc + 4), pc + 4).op)) return false;
                seen.insert(pc + 4);
                if (f == Flow::Branch || f == Flow::BranchLikely || f == Flow::Jump) {
                    if (!valid_target(insn.branch_target_of())) return false;
                    pending.push_back(insn.branch_target_of());
                }
                if (f == Flow::Jump || f == Flow::JumpRegister) break;
                pc += 8;
            }
        }
        return true;
    }

private:
    void walk(std::uint32_t pc) {
        while (valid_target(pc) && !p_.is_code(pc)) {
            const Insn insn = decode(*image_.word(pc), pc);
            if (insn.op == Op::Invalid) return;
            mark(pc);
            const Flow f = flow(insn.op);
            if (f == Flow::Stop) return;
            if (insn.op == Op::Syscall) p_.entries.insert(pc + 4);
            if (f == Flow::None) { pc += 4; continue; }
            if (valid_target(pc + 4) && decode(*image_.word(pc + 4), pc + 4).op != Op::Invalid) mark(pc + 4);
            switch (f) {
            case Flow::Branch:
            case Flow::BranchLikely:
                local(insn.branch_target_of(), pc);
                break;
            case Flow::Jump:
                local(insn.branch_target_of(), pc);
                return;
            case Flow::Call:
                strong(insn.branch_target_of(), true);
                p_.entries.insert(pc + 8);
                break;
            case Flow::CallRegister:
                p_.entries.insert(pc + 8);
                break;
            case Flow::JumpRegister:
                return;
            default:
                break;
            }
            pc += 8;
        }
    }

    void local(std::uint32_t target, std::uint32_t from) {
        if (!valid_target(target)) return;
        if (target <= from) p_.entries.insert(target);
        if (!p_.is_code(target)) queue_.push_back(target);
    }

    void mark(std::uint32_t pc) { p_.code[(pc - p_.base) >> 2] = 1; }

    const Image &image_;
    Program &p_;
    std::deque<std::uint32_t> queue_;
};

std::vector<std::uint32_t> address_constants(const Image &image, const Program &p) {
    std::vector<std::uint32_t> found;
    for (std::uint32_t pc = p.base; pc < p.end; pc += 4) {
        if (!p.is_code(pc)) continue;
        const Insn lui = decode(*image.word(pc), pc);
        if (lui.op != Op::Lui) continue;
        for (std::uint32_t next = pc + 4; next < pc + 40 && p.is_code(next); next += 4) {
            const Insn insn = decode(*image.word(next), next);
            if ((insn.op == Op::Addiu || insn.op == Op::Ori) && insn.rs() == lui.rt()) {
                const std::uint32_t high = lui.uimm() << 16;
                found.push_back(insn.op == Op::Addiu ? high + static_cast<std::uint32_t>(insn.simm()) : high | insn.uimm());
            }
            if (insn.rt() == lui.rt() && insn.op == Op::Lui) break;
        }
    }
    return found;
}

}

Program analyse(const Image &image, const std::vector<std::uint32_t> &extra_entries) {
    Program p;
    p.base = UINT32_MAX;
    for (const auto &segment : image.segments()) {
        if (!segment.executable) continue;
        p.base = std::min(p.base, segment.address & ~3u);
        p.end = std::max<std::uint32_t>(p.end, segment.address + static_cast<std::uint32_t>(segment.bytes.size()));
    }
    p.end &= ~3u;
    p.code.assign((p.end - p.base) >> 2, 0);

    Explorer explorer(image, p);
    explorer.strong(image.entry(), true);
    for (const auto &[address, name] : image.symbols()) explorer.strong(address, true);
    for (std::uint32_t address : extra_entries) explorer.strong(address, true);
    explorer.drain();

    std::uint32_t text_begin = p.end, text_end = p.base;
    for (std::uint32_t pc = p.base; pc < p.end; pc += 4) {
        if (p.is_code(pc)) {
            text_begin = std::min(text_begin, pc);
            text_end = pc + 4;
        }
    }

    for (int round = 0; round < 8; ++round) {
        std::vector<std::uint32_t> candidates = address_constants(image, p);
        for (const auto &segment : image.segments())
            for (std::size_t offset = 0; offset + 4 <= segment.bytes.size(); offset += 4)
                candidates.push_back(*image.word(segment.address + static_cast<std::uint32_t>(offset)));
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        std::size_t added = 0;
        for (std::uint32_t target : candidates) {
            if (target < text_begin || target >= text_end || !explorer.valid_target(target)) continue;
            if (p.entries.count(target) || p.rejected.count(target)) continue;
            if (p.is_code(target) || explorer.tentative(target)) {
                p.entries.insert(target);
                if (!p.is_code(target)) explorer.strong(target, false);
                ++added;
            } else {
                p.rejected.insert(target);
            }
        }
        explorer.drain();
        if (!added) break;
    }
    return p;
}

}
