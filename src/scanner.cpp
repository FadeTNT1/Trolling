#include "scanner.h"

#include <sstream>
#include <iostream>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────
ParsedPattern ParsePattern(const std::string& pattern)
{
    ParsedPattern p;
    std::istringstream ss(pattern);
    std::string token;

    while (ss >> token) {
        if (token == "?" || token == "??") {
            p.bytes.push_back(0x00);
            p.mask.push_back(false);
        } else {
            p.bytes.push_back(
                static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
            p.mask.push_back(true);
        }
    }
    return p;
}

// ── Internal: scan a local buffer for the pattern ────────────────────
static void ScanBuffer(const uint8_t* buf, size_t bufSize,
                       uintptr_t baseAddr,
                       const ParsedPattern& pat,
                       std::vector<ScanResult>& results)
{
    if (bufSize < pat.bytes.size()) return;

    size_t patLen = pat.bytes.size();
    size_t limit  = bufSize - patLen;

    for (size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (pat.mask[j] && buf[i + j] != pat.bytes[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            results.push_back({ baseAddr + i });
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
std::vector<ScanResult> PatternScan(HANDLE process, const ParsedPattern& pattern)
{
    std::vector<ScanResult> results;

    if (pattern.bytes.empty()) return results;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t end  = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    MEMORY_BASIC_INFORMATION mbi{};
    std::vector<uint8_t> buf;

    while (addr < end) {
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(addr),
                           &mbi, sizeof(mbi)) == 0)
            break;

        // Only scan committed, readable regions
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect == PAGE_READWRITE      ||
             mbi.Protect == PAGE_READONLY        ||
             mbi.Protect == PAGE_EXECUTE_READ    ||
             mbi.Protect == PAGE_EXECUTE_READWRITE ||
             mbi.Protect == PAGE_WRITECOPY       ||
             mbi.Protect == PAGE_EXECUTE_WRITECOPY))
        {
            size_t regionSize = mbi.RegionSize;
            buf.resize(regionSize);

            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(process,
                                  reinterpret_cast<LPCVOID>(addr),
                                  buf.data(), regionSize, &bytesRead)
                && bytesRead > 0)
            {
                ScanBuffer(buf.data(), bytesRead,
                           addr, pattern, results);
            }
        }

        addr += mbi.RegionSize;
    }

    return results;
}

// ─────────────────────────────────────────────────────────────────────
std::vector<ScanResult> PatternScanRange(HANDLE process,
                                          const ParsedPattern& pattern,
                                          uintptr_t start, size_t size)
{
    std::vector<ScanResult> results;
    if (pattern.bytes.empty() || size == 0) return results;

    std::vector<uint8_t> buf(size);
    SIZE_T bytesRead = 0;

    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(start),
                          buf.data(), size, &bytesRead)
        && bytesRead > 0)
    {
        ScanBuffer(buf.data(), bytesRead, start, pattern, results);
    }

    return results;
}

// ─────────────────────────────────────────────────────────────────────
uintptr_t ResolveRIP(HANDLE process, uintptr_t instrAddr,
                     int dispOffset, int instrLen)
{
    int32_t disp = 0;
    SIZE_T bytesRead = 0;

    if (!ReadProcessMemory(process,
                           reinterpret_cast<LPCVOID>(instrAddr + dispOffset),
                           &disp, sizeof(disp), &bytesRead)
        || bytesRead != sizeof(disp))
    {
        return 0;
    }

    return instrAddr + instrLen + disp;
}
