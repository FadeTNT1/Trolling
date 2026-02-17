#include "overlay.h"
#include "mc_process.h"
#include "scanner.h"
#include "entity.h"
#include "esp.h"

#include <imgui.h>
#include <iostream>
#include <string>
#include <sstream>
#include <cstdio>
#include <thread>
#include <chrono>

int main()
{
    std::cout << "=== WD-42 ===\n\n";

    // ── Retry loop: smart Minecraft detection every 2 seconds ────────
    ProcessInfo proc{};
    std::cout << "[main] Searching for Minecraft (javaw.exe)...\n";

    while (proc.pid == 0) {
        proc = FindMinecraft();
        if (proc.pid != 0) break;
        std::cout << "[main] Retrying in 2 seconds... (Ctrl+C to cancel)\n\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // ── Init overlay ─────────────────────────────────────────────────
    Overlay overlay;
    if (!overlay.Init(GetModuleHandleW(nullptr))) {
        std::cerr << "[main] Overlay init failed\n";
        if (proc.handle) CloseHandle(proc.handle);
        return 1;
    }

    // ── Entity reader ────────────────────────────────────────────────
    EntityReader entityReader;

    // ── ESP ──────────────────────────────────────────────────────────
    EspConfig espCfg;
    bool f3WasDown = false;

    // ── State ────────────────────────────────────────────────────────
    char addrBuf[32]   = "0x0";
    char aobBuf[256]   = "48 8B 05 ?? ?? ?? ?? 48 85 C0";
    char chainBaseBuf[20] = "0x0";
    char chainOffBuf[128] = "0x10,0x48,0x20";
    int  readSize       = 4;
    bool insertWasDown  = false;
    bool showModules    = false;
    bool showCmdLine    = false;

    // Scanner state
    std::vector<ScanResult> scanResults;
    int selectedResult = 0;

    snprintf(addrBuf, sizeof(addrBuf), "0x%llX",
             static_cast<unsigned long long>(proc.base));

    // ── Main loop ────────────────────────────────────────────────────
    while (overlay.running) {
        if (!overlay.PumpMessages()) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

        // INSERT toggle (edge-triggered)
        bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (insertDown && !insertWasDown)
            overlay.ToggleInteraction();
        insertWasDown = insertDown;

        // F3 toggle ESP (edge-triggered)
        bool f3Down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3Down && !f3WasDown) {
            espCfg.enabled = !espCfg.enabled;
            std::cout << "[esp] ESP " << (espCfg.enabled ? "ON" : "OFF") << "\n";
        }
        f3WasDown = f3Down;

        // Track Minecraft window
        RECT targetRect{};
        HWND targetHwnd = GetTargetWindow(L"Minecraft");
        if (targetHwnd) {
            targetRect = GetTargetRect(targetHwnd);
            overlay.MatchWindow(targetRect);
        }

        // ── Render ───────────────────────────────────────────────────
        overlay.BeginFrame();

        // ── ESP: draw boxes on the background draw list ──────────────
        {
            auto ents = entityReader.GetEntities();
            float tw = static_cast<float>(targetRect.right  - targetRect.left);
            float th = static_cast<float>(targetRect.bottom - targetRect.top);
            // Overlay is positioned at targetRect, so ESP coords are
            // relative to (0,0) of the overlay = targetRect origin.
            DrawEntityESP(ents, espCfg, 0, 0, tw, th);
        }

        ImGui::SetNextWindowBgAlpha(0.90f);
        ImGui::SetNextWindowSize({460, 600}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("WD-42 Panel")) {

            // ── Process Info (always visible) ────────────────────────
            ImGui::TextColored({0.4f,0.8f,1.0f,1}, "Minecraft Process");
            ImGui::Separator();
            ImGui::Text("PID: %lu  Base: 0x%llX  Version: %s",
                        proc.pid,
                        static_cast<unsigned long long>(proc.base),
                        proc.version.c_str());
            ImGui::Text("Method: %s  Handle: %s",
                        proc.detectionMethod.c_str(),
                        proc.handle ? "OK" : "FAILED");

            if (ImGui::Button("Re-detect")) {
                entityReader.Stop();
                if (proc.handle) CloseHandle(proc.handle);
                proc = FindMinecraft();
                scanResults.clear();
                if (proc.pid)
                    snprintf(addrBuf, sizeof(addrBuf), "0x%llX",
                             static_cast<unsigned long long>(proc.base));
            }
            ImGui::SameLine();
            ImGui::Checkbox("Modules", &showModules);
            ImGui::SameLine();
            ImGui::Checkbox("CmdLine", &showCmdLine);

            if (showModules && !proc.mcModules.empty()) {
                for (auto& mod : proc.mcModules) {
                    auto pos = mod.find_last_of(L'\\');
                    std::wstring name = (pos != std::wstring::npos)
                        ? mod.substr(pos + 1) : mod;
                    ImGui::BulletText("%ls", name.c_str());
                }
            }
            if (showCmdLine && !proc.cmdLine.empty()) {
                std::string narrow;
                narrow.resize(proc.cmdLine.size());
                for (size_t ci = 0; ci < proc.cmdLine.size(); ++ci)
                    narrow[ci] = static_cast<char>(proc.cmdLine[ci] & 0x7F);
                if (narrow.size() > 512) narrow.resize(512);
                ImGui::TextWrapped("%s...", narrow.c_str());
            }

            ImGui::Separator();

            // ── Tabs ─────────────────────────────────────────────────
            if (ImGui::BeginTabBar("MainTabs")) {

                // ============ TAB: Entity Reader =====================
                if (ImGui::BeginTabItem("Entities")) {
                    ImGui::TextColored({0.4f,0.8f,1.0f,1},
                        "JVM Entity Reader");
                    ImGui::Separator();

                    std::string readerStatus = entityReader.GetStatus();
                    ImGui::Text("Status: %s", readerStatus.c_str());

                    // ── JVM Oop config ───────────────────────────────
                    if (ImGui::TreeNode("JVM Compressed Oops")) {
                        ImGui::Checkbox("Compressed", &entityReader.oops.compressed);
                        ImGui::InputInt("Shift", &entityReader.oops.shift);
                        // heapBase as hex input
                        static char hbBuf[20] = "0x0";
                        ImGui::InputText("Heap Base", hbBuf, sizeof(hbBuf));
                        entityReader.oops.heapBase =
                            std::strtoull(hbBuf, nullptr, 16);
                        ImGui::TreePop();
                    }

                    // ── Pointer chain ────────────────────────────────
                    if (ImGui::TreeNodeEx("Pointer Chain",
                            ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::InputText("Chain Base", chainBaseBuf,
                                         sizeof(chainBaseBuf));
                        ImGui::InputText("Offsets (hex,csv)",
                                         chainOffBuf, sizeof(chainOffBuf));
                        ImGui::TextWrapped(
                            "Format: base -> [+off0] -> [+off1] -> entity list. "
                            "Discover with Cheat Engine pointer scan.");

                        // Parse into offsets struct
                        entityReader.offsets.chainBase =
                            std::strtoull(chainBaseBuf, nullptr, 16);
                        {
                            entityReader.offsets.chainOffsets.clear();
                            std::string s(chainOffBuf);
                            std::istringstream ss(s);
                            std::string tok;
                            while (std::getline(ss, tok, ',')) {
                                if (tok.empty()) continue;
                                entityReader.offsets.chainOffsets.push_back(
                                    static_cast<int>(
                                        std::strtol(tok.c_str(), nullptr, 16)));
                            }
                        }
                        ImGui::TreePop();
                    }

                    // ── Entity offsets ────────────────────────────────
                    if (ImGui::TreeNode("Entity Field Offsets")) {
                        auto& o = entityReader.offsets;
                        ImGui::InputInt("List size off",  &o.listSizeOffset);
                        ImGui::InputInt("List array off", &o.listArrayOffset);
                        ImGui::InputInt("Array data off", &o.arrayDataOffset);
                        ImGui::Separator();
                        ImGui::InputInt("posX off", &o.posXOffset);
                        ImGui::InputInt("posY off", &o.posYOffset);
                        ImGui::InputInt("posZ off", &o.posZOffset);
                        ImGui::Separator();
                        ImGui::InputInt("BB ref off",  &o.bbRefOffset);
                        ImGui::InputInt("BB minX off", &o.bbMinXOffset);
                        ImGui::InputInt("BB minY off", &o.bbMinYOffset);
                        ImGui::InputInt("BB minZ off", &o.bbMinZOffset);
                        ImGui::InputInt("BB maxX off", &o.bbMaxXOffset);
                        ImGui::InputInt("BB maxY off", &o.bbMaxYOffset);
                        ImGui::InputInt("BB maxZ off", &o.bbMaxZOffset);
                        ImGui::InputInt("Max entities", &o.maxEntities);
                        ImGui::TreePop();
                    }

                    ImGui::Separator();

                    // ── Controls ─────────────────────────────────────
                    if (!entityReader.IsRunning()) {
                        if (ImGui::Button("Start Reader") && proc.handle) {
                            entityReader.Start(proc.handle);
                        }
                    } else {
                        if (ImGui::Button("Stop Reader")) {
                            entityReader.Stop();
                        }
                    }

                    if (entityReader.IsRunning()) {
                        ImGui::SameLine();
                        bool enabled = entityReader.entityReadEnabled.load();
                        if (ImGui::Checkbox("Read Entities", &enabled))
                            entityReader.entityReadEnabled.store(enabled);

                        ImGui::SameLine();
                        if (ImGui::Button("Scan Strings"))
                            entityReader.RequestStringScan();
                    }

                    ImGui::SliderInt("Interval (ms)",
                                     &entityReader.readIntervalMs, 10, 500);

                    // ── ESP config ────────────────────────────────────
                    ImGui::Separator();
                    ImGui::TextColored({0.4f,0.8f,1.0f,1},
                        "ESP Overlay (F3 toggle)");
                    ImGui::Separator();
                    ImGui::Checkbox("ESP Enabled", &espCfg.enabled);
                    ImGui::SameLine();
                    ImGui::Checkbox("Labels", &espCfg.showLabels);
                    ImGui::SameLine();
                    ImGui::Checkbox("Distance", &espCfg.showDistance);
                    ImGui::Checkbox("Snap Lines", &espCfg.showSnaplines);
                    ImGui::ColorEdit4("Box Color",
                        &espCfg.boxR, ImGuiColorEditFlags_NoInputs);
                    ImGui::SliderFloat("Thickness",
                        &espCfg.thickness, 1.0f, 5.0f);
                    ImGui::SliderFloat("Max Dist",
                        &espCfg.maxDrawDist, 16.0f, 512.0f);

                    if (ImGui::TreeNode("Camera (Identity Placeholder)")) {
                        ImGui::DragFloat3("Position",
                            &espCfg.camPos.x, 0.5f);
                        ImGui::SliderFloat("Yaw",
                            &espCfg.camYaw, -180.0f, 180.0f);
                        ImGui::SliderFloat("Pitch",
                            &espCfg.camPitch, -90.0f, 90.0f);
                        ImGui::SliderFloat("FOV",
                            &espCfg.fovY, 30.0f, 120.0f);
                        ImGui::TextWrapped(
                            "Set these to your player's actual position "
                            "and rotation from memory or F3 screen. "
                            "Without correct camera data, boxes won't "
                            "align with the game view.");
                        ImGui::TreePop();
                    }

                    // ── String scan results ──────────────────────────
                    auto finds = entityReader.GetStringFinds();
                    if (!finds.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored({0.4f,1.0f,0.4f,1},
                            "Class Strings Found: %zu", finds.size());

                        int showN = (static_cast<int>(finds.size()) < 32)
                            ? static_cast<int>(finds.size()) : 32;
                        for (int i = 0; i < showN; ++i) {
                            ImGui::Text("  0x%llX  %s",
                                static_cast<unsigned long long>(
                                    finds[i].address),
                                finds[i].text.c_str());
                        }
                        if (finds.size() > 32)
                            ImGui::Text("  ... +%zu more",
                                        finds.size() - 32);
                    }

                    // ── Entity data ──────────────────────────────────
                    auto ents = entityReader.GetEntities();
                    if (!ents.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored({0.4f,1.0f,0.4f,1},
                            "Entities: %zu", ents.size());

                        int validCount = 0;
                        for (auto& e : ents)
                            if (e.valid) ++validCount;

                        ImGui::Text("Valid: %d / %zu",
                                    validCount, ents.size());

                        if (ImGui::BeginChild("EntityList",
                                {0, 200}, true))
                        {
                            for (auto& e : ents) {
                                if (!e.valid) continue;
                                ImGui::Text("#%-3d X:%.2f Y:%.2f Z:%.2f",
                                    e.index, e.posX, e.posY, e.posZ);
                                if (ImGui::IsItemHovered() &&
                                    (e.bbMaxX != 0 || e.bbMaxY != 0))
                                {
                                    ImGui::SetTooltip(
                                        "BB: [%.1f,%.1f,%.1f]-[%.1f,%.1f,%.1f]",
                                        e.bbMinX, e.bbMinY, e.bbMinZ,
                                        e.bbMaxX, e.bbMaxY, e.bbMaxZ);
                                }
                            }
                        }
                        ImGui::EndChild();
                    }

                    ImGui::EndTabItem();
                }

                // ============ TAB: AOB Scanner ========================
                if (ImGui::BeginTabItem("Scanner")) {
                    ImGui::TextColored({0.4f,0.8f,1.0f,1},
                        "AOB Pattern Scanner");
                    ImGui::Separator();

                    ImGui::InputText("Pattern", aobBuf, sizeof(aobBuf));

                    if (ImGui::Button("Scan") && proc.handle) {
                        std::cout << "[scanner] Scanning: " << aobBuf << "\n";
                        auto pat = ParsePattern(aobBuf);
                        scanResults = PatternScan(proc.handle, pat);
                        selectedResult = 0;
                        std::cout << "[scanner] " << scanResults.size()
                                  << " results\n";
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear")) {
                        scanResults.clear();
                        selectedResult = 0;
                    }

                    if (!scanResults.empty()) {
                        ImGui::Text("Results: %zu", scanResults.size());

                        int show = (static_cast<int>(scanResults.size()) < 64)
                            ? static_cast<int>(scanResults.size()) : 64;
                        for (int i = 0; i < show; ++i) {
                            char label[64];
                            snprintf(label, sizeof(label), "0x%llX",
                                     static_cast<unsigned long long>(
                                         scanResults[i].address));
                            if (ImGui::Selectable(label,
                                    selectedResult == i))
                            {
                                selectedResult = i;
                                snprintf(addrBuf, sizeof(addrBuf),
                                    "0x%llX",
                                    static_cast<unsigned long long>(
                                        scanResults[i].address));
                            }
                        }
                        if (scanResults.size() > 64)
                            ImGui::Text("... +%zu more",
                                        scanResults.size() - 64);
                    } else {
                        ImGui::TextColored({0.5f,0.5f,0.5f,1},
                            "No results");
                    }

                    ImGui::EndTabItem();
                }

                // ============ TAB: Memory Reader ======================
                if (ImGui::BeginTabItem("Memory")) {
                    ImGui::TextColored({0.4f,0.8f,1.0f,1},
                        "Memory Reader");
                    ImGui::Separator();

                    ImGui::InputText("Address", addrBuf, sizeof(addrBuf));
                    ImGui::SliderInt("Bytes", &readSize, 1, 8);

                    uintptr_t memAddr =
                        std::strtoull(addrBuf, nullptr, 16);

                    if (proc.handle && memAddr) {
                        ImGui::Text("Reading 0x%llX (%d bytes):",
                            static_cast<unsigned long long>(memAddr),
                            readSize);

                        switch (readSize) {
                        case 1:
                            if (auto v = ReadMemory<uint8_t>(
                                    proc.handle, memAddr))
                                ImGui::Text("  uint8  = %u (0x%02X)",
                                            *v, *v);
                            else
                                ImGui::TextColored({1,0,0,1},
                                    "  read failed");
                            break;
                        case 2:
                            if (auto v = ReadMemory<uint16_t>(
                                    proc.handle, memAddr))
                                ImGui::Text("  uint16 = %u (0x%04X)",
                                            *v, *v);
                            else
                                ImGui::TextColored({1,0,0,1},
                                    "  read failed");
                            break;
                        case 4:
                            if (auto v = ReadMemory<int32_t>(
                                    proc.handle, memAddr))
                                ImGui::Text("  int32  = %d (0x%08X)",
                                            *v, *v);
                            else
                                ImGui::TextColored({1,0,0,1},
                                    "  read failed");
                            if (auto v = ReadMemory<float>(
                                    proc.handle, memAddr))
                                ImGui::Text("  float  = %.4f", *v);
                            break;
                        case 8:
                            if (auto v = ReadMemory<int64_t>(
                                    proc.handle, memAddr))
                                ImGui::Text(
                                    "  int64  = %lld (0x%llX)", *v,
                                    static_cast<unsigned long long>(*v));
                            else
                                ImGui::TextColored({1,0,0,1},
                                    "  read failed");
                            if (auto v = ReadMemory<double>(
                                    proc.handle, memAddr))
                                ImGui::Text("  double = %.6f", *v);
                            break;
                        default:
                            ImGui::Text("  (select 1/2/4/8)");
                        }

                        // Hex dump
                        auto bytes = ReadBytes(proc.handle, memAddr, 32);
                        if (!bytes.empty()) {
                            ImGui::Separator();
                            ImGui::Text("Hex dump (+32 bytes):");
                            std::string hexLine, asciiLine;
                            for (size_t i = 0; i < bytes.size(); ++i) {
                                char hex[4];
                                snprintf(hex, sizeof(hex), "%02X ",
                                         bytes[i]);
                                hexLine += hex;
                                asciiLine +=
                                    (bytes[i] >= 0x20 && bytes[i] < 0x7F)
                                    ? static_cast<char>(bytes[i]) : '.';
                                if ((i + 1) % 16 == 0 ||
                                    i + 1 == bytes.size())
                                {
                                    ImGui::Text("  %s |%s|",
                                        hexLine.c_str(),
                                        asciiLine.c_str());
                                    hexLine.clear();
                                    asciiLine.clear();
                                }
                            }
                        }
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            // ── Footer ───────────────────────────────────────────────
            ImGui::Separator();
            ImGui::TextColored({0.5f,0.5f,0.5f,1},
                "INSERT = click-through (%s)  |  F3 = ESP (%s)  |  ESC = quit",
                overlay.clickThrough ? "ON" : "OFF",
                espCfg.enabled ? "ON" : "OFF");
        }
        ImGui::End();

        overlay.EndFrame();
    }

    // ── Cleanup ──────────────────────────────────────────────────────
    entityReader.Stop();
    overlay.Shutdown();
    if (proc.handle) CloseHandle(proc.handle);

    std::cout << "[main] Exiting\n";
    return 0;
}
