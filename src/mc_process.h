#pragma once

#include <Windows.h>
#include <optional>
#include <string>
#include <vector>

struct ProcessInfo {
    DWORD       pid    = 0;
    HANDLE      handle = nullptr;
    uintptr_t   base   = 0;              // base address of javaw.exe module
    std::string version;                  // e.g. "1.21.5" or "unknown"
    std::string detectionMethod;          // how we confirmed it's Minecraft
    std::wstring cmdLine;                 // full command line of the process
    std::vector<std::wstring> mcModules;  // Minecraft-related DLLs found
};

// ── Smart Minecraft Detection ────────────────────────────────────────
// Enumerates all javaw.exe processes, scores them by Minecraft
// indicators (command line, loaded modules, window title), picks the
// best candidate, and extracts a version hint.
ProcessInfo FindMinecraft();

// ── Memory Helpers ───────────────────────────────────────────────────

// Read a value of type T from the target process memory.
template <typename T>
std::optional<T> ReadMemory(HANDLE process, uintptr_t address);

// Read a block of raw bytes.
std::vector<uint8_t> ReadBytes(HANDLE process, uintptr_t address, size_t count);

// Locate a top-level window by exact title.
HWND GetTargetWindow(const wchar_t* windowTitle);

// Get the screen-space rectangle of a window.
RECT GetTargetRect(HWND hwnd);

// ── Template implementation ──────────────────────────────────────────
template <typename T>
std::optional<T> ReadMemory(HANDLE process, uintptr_t address)
{
    T value{};
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address),
                          &value, sizeof(T), &bytesRead)
        && bytesRead == sizeof(T))
    {
        return value;
    }
    return std::nullopt;
}
