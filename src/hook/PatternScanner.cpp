#include "PatternScanner.h"

#include <windows.h>
#include <cstring>

namespace {
struct Byte {
    uint8_t value;
    bool wild;
};

std::vector<Byte> Parse(const char* p) {
    std::vector<Byte> out;
    while (*p) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (*p == '?') {
            out.push_back({0, true});
            ++p;
            if (*p == '?') ++p;
            continue;
        }
        char hex[3] = {p[0], p[1], 0};
        out.push_back({(uint8_t)strtoul(hex, nullptr, 16), false});
        p += 2;
    }
    return out;
}
}  // namespace

namespace PatternScanner {

std::vector<uintptr_t> FindAll(uintptr_t begin, size_t size, const char* pattern) {
    std::vector<uintptr_t> hits;
    const std::vector<Byte> pat = Parse(pattern);
    if (pat.empty() || size < pat.size()) return hits;
    const uint8_t* mem = reinterpret_cast<const uint8_t*>(begin);
    const size_t last = size - pat.size();
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        while (j < pat.size() && (pat[j].wild || mem[i + j] == pat[j].value)) ++j;
        if (j == pat.size()) hits.push_back(begin + i);
    }
    return hits;
}

bool TextSection(uintptr_t moduleBase, uintptr_t* begin, size_t* size) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 6) == 0) {
            *begin = moduleBase + sec->VirtualAddress;
            *size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

}  // namespace PatternScanner
