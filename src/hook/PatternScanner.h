#pragma once
#include <cstdint>
#include <vector>

// IDA-style patterns: "55 8B EC ?? ?? 0F 84". '?' or '??' is a wildcard byte.
namespace PatternScanner {
// All matches inside [begin, begin+size).
std::vector<uintptr_t> FindAll(uintptr_t begin, size_t size, const char* pattern);
// The module's .text section bounds (false if not found).
bool TextSection(uintptr_t moduleBase, uintptr_t* begin, size_t* size);
}
