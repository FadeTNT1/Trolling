#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

// ── AOB Pattern Scanner ──────────────────────────────────────────────
// Scans committed memory regions of an external process for byte
// patterns with wildcard support.
//
// Pattern format: "48 8B 05 ?? ?? ?? ?? 48 85 C0"
//   - Two hex chars = exact byte match
//   - "??" or "?"   = wildcard (matches any byte)

struct ScanResult {
    uintptr_t address = 0;
};

// Parse a pattern string into byte array + mask.
// mask[i] == true means bytes[i] must match exactly.
struct ParsedPattern {
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask;    // true = must match, false = wildcard
};

ParsedPattern ParsePattern(const std::string& pattern);

// Scan all committed, readable regions of `process` for `pattern`.
// Returns addresses of all matches.
std::vector<ScanResult> PatternScan(HANDLE process, const ParsedPattern& pattern);

// Scan only within a specific address range.
std::vector<ScanResult> PatternScanRange(HANDLE process, const ParsedPattern& pattern,
                                         uintptr_t start, size_t size);

// Resolve a RIP-relative address.
// Given a match at `instrAddr` where the 32-bit displacement is at
// offset `dispOffset` within the pattern, and the instruction is
// `instrLen` bytes total, compute the absolute target:
//   target = instrAddr + instrLen + *(int32_t*)(instrAddr + dispOffset)
// Reads the displacement from the remote process.
uintptr_t ResolveRIP(HANDLE process, uintptr_t instrAddr,
                     int dispOffset, int instrLen);
