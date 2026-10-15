#include "psprecomp/elf32.hpp"
#include "psprecomp/decoder.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: chili_opcode_audit <BOOT.BIN> <unsupported.csv>\n";
            return 2;
        }
        const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
        psprecomp::GuestMemory memory;
        (void)elf.load_and_relocate(memory);
        std::map<std::uint32_t, std::pair<std::size_t, std::uint32_t>> words;
        std::map<std::string, std::size_t> mnemonics;
        std::size_t total = 0, unsupported = 0;
        for (const auto &section : elf.sections()) {
            if (section.name != ".text" && section.name != ".init" && section.name != ".fini") continue;
            const auto address = elf.section_runtime_address(section);
            for (std::uint32_t offset = 0; offset + 4u <= section.size; offset += 4u) {
                const auto pc = address + offset;
                const auto word = memory.load32(pc);
                const auto instruction = psprecomp::decode_allegrex(word);
                ++total;
                ++mnemonics[instruction.mnemonic];
                if (instruction.kind != psprecomp::OpcodeKind::Unsupported &&
                    instruction.kind != psprecomp::OpcodeKind::Vfpu) continue;
                ++unsupported;
                auto &entry = words[word];
                if (!entry.first) entry.second = pc;
                ++entry.first;
            }
        }
        std::vector<std::pair<std::uint32_t, std::pair<std::size_t, std::uint32_t>>> sorted(words.begin(), words.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.first > b.second.first; });
        std::ofstream csv(argv[2]);
        csv << "word,count,first_pc,mnemonic\n";
        for (const auto &[word, entry] : sorted)
            csv << psprecomp::hex32(word) << ',' << entry.first << ','
                << psprecomp::hex32(entry.second) << ',' << psprecomp::decode_allegrex(word).mnemonic << '\n';
        std::cout << "Text words=" << total << " unsupported=" << unsupported << '\n';
        for (const auto &[name, count] : mnemonics)
            if (name.starts_with("vfpu") || name.starts_with("unknown"))
                std::cout << name << '=' << count << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
