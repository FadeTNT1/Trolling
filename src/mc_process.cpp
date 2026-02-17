#include "mc_process.h"

#include <Psapi.h>
#include <winternl.h>
#include <iostream>
#include <algorithm>
#include <regex>
#include <cctype>

// ── NtQueryInformationProcess (loaded at runtime from ntdll) ─────────
typedef NTSTATUS(NTAPI* NtQueryInformationProcessFn)(
    HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

static NtQueryInformationProcessFn GetNtQueryInformationProcess()
{
    static auto fn = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "NtQueryInformationProcess"));
    return fn;
}

// ── Helpers ──────────────────────────────────────────────────────────

static bool PathEndsWith(const std::wstring& path, const std::wstring& name)
{
    auto pos = path.find_last_of(L'\\');
    std::wstring file = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
    return _wcsicmp(file.c_str(), name.c_str()) == 0;
}

static std::wstring ToLower(std::wstring s)
{
    for (auto& c : s) c = towlower(c);
    return s;
}

static bool ContainsI(const std::wstring& haystack, const wchar_t* needle)
{
    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

static std::string WideToNarrow(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(),
                                  static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(),
                        static_cast<int>(ws.size()),
                        s.data(), len, nullptr, nullptr);
    return s;
}

// ── Read command line from a remote process via PEB ──────────────────
static std::wstring ReadRemoteCmdLine(HANDLE hProc)
{
    auto NtQueryInfo = GetNtQueryInformationProcess();
    if (!NtQueryInfo) return {};

    // 1. Get PEB address
    PROCESS_BASIC_INFORMATION pbi{};
    ULONG retLen = 0;
    NTSTATUS st = NtQueryInfo(hProc, ProcessBasicInformation,
                              &pbi, sizeof(pbi), &retLen);
    if (st != 0 || !pbi.PebBaseAddress) return {};

    // 2. Read ProcessParameters pointer from PEB.
    //    On x64: PEB + 0x20 = RTL_USER_PROCESS_PARAMETERS*
    uintptr_t pebAddr = reinterpret_cast<uintptr_t>(pbi.PebBaseAddress);
    uintptr_t paramsPtr = 0;
    SIZE_T br = 0;

    if (!ReadProcessMemory(hProc,
            reinterpret_cast<LPCVOID>(pebAddr + 0x20),
            &paramsPtr, sizeof(paramsPtr), &br)
        || br != sizeof(paramsPtr) || !paramsPtr)
        return {};

    // 3. Read CommandLine UNICODE_STRING from RTL_USER_PROCESS_PARAMETERS.
    //    On x64: offset 0x70 = CommandLine (UNICODE_STRING: Length, MaxLen, Buffer)
    struct UnicodeStr { USHORT Length; USHORT MaxLength; ULONG pad; PVOID Buffer; };
    UnicodeStr ustr{};

    if (!ReadProcessMemory(hProc,
            reinterpret_cast<LPCVOID>(paramsPtr + 0x70),
            &ustr, sizeof(ustr), &br)
        || br != sizeof(ustr) || !ustr.Buffer || ustr.Length == 0)
        return {};

    // 4. Read the actual string buffer
    std::wstring cmdLine;
    cmdLine.resize(ustr.Length / sizeof(wchar_t));

    if (!ReadProcessMemory(hProc,
            reinterpret_cast<LPCVOID>(ustr.Buffer),
            cmdLine.data(), ustr.Length, &br)
        || br == 0)
        return {};

    return cmdLine;
}

// ── Check loaded modules for Minecraft-related DLLs ─────────────────
static std::vector<std::wstring> FindMcModules(HANDLE hProc)
{
    std::vector<std::wstring> found;

    HMODULE mods[2048];
    DWORD cbNeeded = 0;
    if (!EnumProcessModulesEx(hProc, mods, sizeof(mods), &cbNeeded,
                              LIST_MODULES_ALL))
        return found;

    DWORD modCount = cbNeeded / sizeof(HMODULE);
    wchar_t modName[MAX_PATH];

    for (DWORD i = 0; i < modCount; ++i) {
        if (GetModuleFileNameExW(hProc, mods[i], modName, MAX_PATH)) {
            std::wstring name(modName);
            std::wstring lower = ToLower(name);

            // LWJGL native libraries — strong Minecraft signal
            if (lower.find(L"lwjgl") != std::wstring::npos)
                found.push_back(name);
            // OpenAL (bundled with MC)
            else if (lower.find(L"openal") != std::wstring::npos)
                found.push_back(name);
            // GLFW (used by modern MC via LWJGL)
            else if (lower.find(L"glfw") != std::wstring::npos)
                found.push_back(name);
        }
    }
    return found;
}

// ── EnumWindows callback: find a window owned by a specific PID ──────
struct WindowSearch {
    DWORD  pid;
    HWND   bestHwnd   = nullptr;
    std::wstring title;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* ws = reinterpret_cast<WindowSearch*>(lParam);

    DWORD winPid = 0;
    GetWindowThreadProcessId(hwnd, &winPid);
    if (winPid != ws->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t buf[512] = {};
    GetWindowTextW(hwnd, buf, 512);
    std::wstring title(buf);

    if (title.empty()) return TRUE;

    // Prefer windows with "Minecraft" in the title
    if (ContainsI(title, L"Minecraft")) {
        ws->bestHwnd = hwnd;
        ws->title    = title;
        return FALSE; // stop — we found the best one
    }

    // Otherwise keep the first visible titled window
    if (!ws->bestHwnd) {
        ws->bestHwnd = hwnd;
        ws->title    = title;
    }
    return TRUE;
}

// ── Extract version string from command line or window title ─────────
static std::string ExtractVersion(const std::wstring& cmdLine,
                                   const std::wstring& windowTitle)
{
    // Strategy 1: Look for --version argument in command line
    // Common in Prism/MultiMC: ... --version 1.21.5 ...
    {
        std::wstring lower = ToLower(cmdLine);
        size_t pos = lower.find(L"--version");
        if (pos != std::wstring::npos) {
            pos += 9; // skip "--version"
            while (pos < cmdLine.size() && cmdLine[pos] == L' ') ++pos;
            size_t end = cmdLine.find(L' ', pos);
            if (end == std::wstring::npos) end = cmdLine.size();
            std::wstring ver = cmdLine.substr(pos, end - pos);
            if (!ver.empty())
                return WideToNarrow(ver);
        }
    }

    // Strategy 2: Regex for version-like pattern in command line
    // Matches things like: 1.21.5, 1.12.2, 1.20.4-fabric, etc.
    {
        std::string narrow = WideToNarrow(cmdLine);
        std::regex re(R"((?:^|\s|[/\\-])(\d+\.\d+(?:\.\d+)?)(?:\s|$|[/\\"-]))");
        std::smatch m;
        std::string bestVersion;

        auto it  = narrow.cbegin();
        auto end = narrow.cend();
        while (std::regex_search(it, end, m, re)) {
            std::string ver = m[1].str();
            // Prefer versions that look like Minecraft (1.x.x where x >= 7)
            if (ver.size() >= 4 && ver[0] == '1' && ver[1] == '.') {
                bestVersion = ver;
            }
            it = m.suffix().first;
        }
        if (!bestVersion.empty()) return bestVersion;
    }

    // Strategy 3: Parse window title (e.g. "Minecraft 1.21.5")
    {
        std::string narrow = WideToNarrow(windowTitle);
        std::regex re(R"((\d+\.\d+(?:\.\d+)?))");
        std::smatch m;
        if (std::regex_search(narrow, m, re))
            return m[1].str();
    }

    return "unknown";
}

// ── Candidate scoring ────────────────────────────────────────────────
struct Candidate {
    DWORD    pid    = 0;
    HANDLE   handle = nullptr;
    uintptr_t base  = 0;
    int      score  = 0;
    std::wstring cmdLine;
    std::wstring windowTitle;
    std::vector<std::wstring> modules;
    SIZE_T   memUsage = 0;
};

// =====================================================================
//  FindMinecraft — the main smart detection function
// =====================================================================
ProcessInfo FindMinecraft()
{
    // ── Enumerate PIDs ───────────────────────────────────────────────
    DWORD pids[4096];
    DWORD bytesReturned = 0;

    if (!EnumProcesses(pids, sizeof(pids), &bytesReturned)) {
        std::cerr << "[process] EnumProcesses failed (error "
                  << GetLastError() << ")\n";
        return {};
    }

    DWORD count = bytesReturned / sizeof(DWORD);
    std::cout << "[process] Scanning " << count << " processes...\n";

    std::vector<Candidate> candidates;

    // ── First pass: find all javaw.exe instances ─────────────────────
    for (DWORD i = 0; i < count; ++i) {
        DWORD pid = pids[i];
        if (pid == 0) continue;

        HANDLE hProc = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
            FALSE, pid);

        if (!hProc) continue;

        wchar_t fullPath[MAX_PATH] = {};
        if (GetModuleFileNameExW(hProc, nullptr, fullPath, MAX_PATH) == 0) {
            CloseHandle(hProc);
            continue;
        }

        if (!PathEndsWith(fullPath, L"javaw.exe") &&
            !PathEndsWith(fullPath, L"java.exe"))
        {
            CloseHandle(hProc);
            continue;
        }

        // Found a Java process — build candidate
        Candidate c;
        c.pid    = pid;
        c.handle = hProc;
        c.score  = 0;

        // Base address
        HMODULE hMod = nullptr;
        DWORD cbNeeded = 0;
        if (EnumProcessModules(hProc, &hMod, sizeof(hMod), &cbNeeded))
            c.base = reinterpret_cast<uintptr_t>(hMod);

        // Memory usage (for tie-breaking — MC is memory hungry)
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc)))
            c.memUsage = pmc.WorkingSetSize;

        // ── Score: command line ──────────────────────────────────────
        c.cmdLine = ReadRemoteCmdLine(hProc);
        if (!c.cmdLine.empty()) {
            std::wstring lower = ToLower(c.cmdLine);

            // Strong indicators in command line
            if (lower.find(L"minecraft") != std::wstring::npos)  c.score += 40;
            if (lower.find(L"net.minecraft") != std::wstring::npos) c.score += 20;
            if (lower.find(L"--version") != std::wstring::npos)  c.score += 10;
            if (lower.find(L"lwjgl") != std::wstring::npos)      c.score += 10;
            if (lower.find(L"authlib") != std::wstring::npos)    c.score += 10;
            if (lower.find(L".minecraft") != std::wstring::npos) c.score += 5;
        }

        // ── Score: loaded modules ────────────────────────────────────
        c.modules = FindMcModules(hProc);
        if (!c.modules.empty()) {
            c.score += 30;  // LWJGL/OpenAL loaded = very strong signal
            c.score += static_cast<int>(c.modules.size()) * 5;
        }

        // ── Score: window title ──────────────────────────────────────
        WindowSearch ws{ pid };
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ws));
        c.windowTitle = ws.title;

        if (ContainsI(c.windowTitle, L"Minecraft"))
            c.score += 50;  // Strongest signal

        std::cout << "[process] Java PID " << pid
                  << "  score=" << c.score
                  << "  mem=" << (c.memUsage / (1024 * 1024)) << " MB";
        if (!c.windowTitle.empty()) {
            std::cout << "  title=\"";
            std::wcout << c.windowTitle;
            std::cout << "\"";
        }
        std::cout << "\n";

        candidates.push_back(std::move(c));
    }

    if (candidates.empty()) {
        std::cout << "[process] No Java processes found.\n";
        return {};
    }

    // ── Pick the best candidate ──────────────────────────────────────
    // Sort by score descending, then by memory usage descending (tie-break)
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.memUsage > b.memUsage;
        });

    Candidate& best = candidates[0];

    // Close handles for all losers
    for (size_t i = 1; i < candidates.size(); ++i)
        CloseHandle(candidates[i].handle);

    if (best.score == 0) {
        std::cout << "[process] Java processes found but none look like Minecraft.\n";
        CloseHandle(best.handle);
        return {};
    }

    // ── Build detection method string ────────────────────────────────
    std::string method;
    if (ContainsI(best.windowTitle, L"Minecraft"))
        method += "window_title ";
    if (ContainsI(best.cmdLine, L"net.minecraft"))
        method += "main_class ";
    if (ContainsI(best.cmdLine, L"minecraft"))
        method += "cmdline ";
    if (!best.modules.empty())
        method += "lwjgl_modules ";
    if (method.empty()) method = "heuristic";

    // ── Extract version ──────────────────────────────────────────────
    std::string version = ExtractVersion(best.cmdLine, best.windowTitle);

    // ── Report ───────────────────────────────────────────────────────
    std::cout << "\n[process] === Detected Minecraft process ===\n"
              << "  PID     : " << best.pid << "\n"
              << "  Base    : 0x" << std::hex << best.base << std::dec << "\n"
              << "  Version : " << version << "\n"
              << "  Score   : " << best.score << "\n"
              << "  Method  : " << method << "\n"
              << "  Memory  : " << (best.memUsage / (1024 * 1024)) << " MB\n"
              << "  Modules : " << best.modules.size() << " MC-related DLLs\n";

    ProcessInfo info;
    info.pid             = best.pid;
    info.handle          = best.handle;
    info.base            = best.base;
    info.version         = version;
    info.detectionMethod = method;
    info.cmdLine         = best.cmdLine;
    info.mcModules       = std::move(best.modules);
    return info;
}

// ── Utility functions ────────────────────────────────────────────────

std::vector<uint8_t> ReadBytes(HANDLE process, uintptr_t address, size_t count)
{
    std::vector<uint8_t> buf(count);
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address),
                          buf.data(), count, &bytesRead))
    {
        buf.resize(bytesRead);
    } else {
        buf.clear();
    }
    return buf;
}

HWND GetTargetWindow(const wchar_t* windowTitle)
{
    return FindWindowW(nullptr, windowTitle);
}

RECT GetTargetRect(HWND hwnd)
{
    RECT rc{};
    if (hwnd)
        GetWindowRect(hwnd, &rc);
    return rc;
}
