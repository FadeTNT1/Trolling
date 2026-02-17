#include "entity.h"
#include "mc_process.h"   // ReadMemory<T>, ReadBytes
#include "scanner.h"   // PatternScan, ParsePattern
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>

// ── Known Minecraft class-name strings (UTF-8) to scan for ───────────
// These live in the JVM metaspace / constant pool.  Finding them proves
// we can locate class metadata and gives anchor addresses to work from.
static const char* kClassSignatures[] = {
    "net/minecraft/client/MinecraftClient",
    "net/minecraft/client/Minecraft",           // MCP / official
    "net/minecraft/entity/Entity",
    "net/minecraft/entity/player/PlayerEntity",
    "net/minecraft/entity/player/EntityPlayer",  // MCP
    "net/minecraft/client/world/ClientWorld",
    "net/minecraft/world/entity/LivingEntity",
    "net/minecraft/world/level/Level",
};

// =====================================================================
//  Lifecycle
// =====================================================================

EntityReader::~EntityReader()
{
    Stop();
}

void EntityReader::Start(HANDLE process)
{
    if (running.load()) return;

    hProcess = process;
    running.store(true);

    worker = std::thread(&EntityReader::WorkerLoop, this);
    std::cout << "[entity] Background reader started\n";
}

void EntityReader::Stop()
{
    running.store(false);
    if (worker.joinable())
        worker.join();
    std::cout << "[entity] Background reader stopped\n";
}

// =====================================================================
//  Thread-safe accessors
// =====================================================================

std::vector<EntityData> EntityReader::GetEntities() const
{
    std::lock_guard<std::mutex> lk(mtx);
    return entities;
}

std::vector<StringFind> EntityReader::GetStringFinds() const
{
    std::lock_guard<std::mutex> lk(mtx);
    return stringFinds;
}

std::string EntityReader::GetStatus() const
{
    std::lock_guard<std::mutex> lk(mtx);
    return status;
}

void EntityReader::RequestStringScan()
{
    stringScanRequested.store(true);
}

// =====================================================================
//  Worker thread
// =====================================================================

void EntityReader::WorkerLoop()
{
    while (running.load()) {
        // Handle one-shot string scan request
        if (stringScanRequested.exchange(false)) {
            {
                std::lock_guard<std::mutex> lk(mtx);
                status = "Scanning for JVM class strings...";
            }
            DoStringScan();
        }

        // Continuous entity reads
        if (entityReadEnabled.load() && offsets.chainBase != 0) {
            DoEntityRead();
        }

        Sleep(static_cast<DWORD>(readIntervalMs));
    }

    std::lock_guard<std::mutex> lk(mtx);
    status = "stopped";
}

// =====================================================================
//  JVM Oop dereference
// =====================================================================

uintptr_t EntityReader::ReadOop(uintptr_t addr) const
{
    if (oops.compressed) {
        // Compressed oop: 4-byte ref, decode = (ref << shift) + heapBase
        auto ref = ReadMemory<uint32_t>(hProcess, addr);
        if (!ref || *ref == 0) return 0;
        return (static_cast<uintptr_t>(*ref) << oops.shift) + oops.heapBase;
    } else {
        // Raw 8-byte pointer
        auto ptr = ReadMemory<uint64_t>(hProcess, addr);
        if (!ptr || *ptr == 0) return 0;
        return static_cast<uintptr_t>(*ptr);
    }
}

// =====================================================================
//  Pointer chain follower
// =====================================================================

uintptr_t EntityReader::FollowChain() const
{
    uintptr_t addr = offsets.chainBase;
    if (addr == 0) return 0;

    for (size_t i = 0; i < offsets.chainOffsets.size(); ++i) {
        // Dereference the current pointer
        auto ptr = ReadMemory<uint64_t>(hProcess, addr);
        if (!ptr || *ptr == 0) return 0;
        addr = static_cast<uintptr_t>(*ptr);

        // Apply the next offset
        addr += offsets.chainOffsets[i];
    }

    return addr;
}

// =====================================================================
//  String scan: find known class names in JVM heap/metaspace
// =====================================================================

void EntityReader::DoStringScan()
{
    std::vector<StringFind> results;

    std::cout << "[entity] Scanning for " << (sizeof(kClassSignatures) / sizeof(kClassSignatures[0]))
              << " known class-name signatures...\n";

    for (const char* sig : kClassSignatures) {
        // Convert the string to a byte pattern (all exact matches)
        std::string patStr;
        for (const char* p = sig; *p; ++p) {
            char hex[4];
            snprintf(hex, sizeof(hex), "%02X ", static_cast<unsigned char>(*p));
            patStr += hex;
        }

        auto pat = ParsePattern(patStr);
        auto hits = PatternScan(hProcess, pat);

        for (auto& h : hits) {
            StringFind sf;
            sf.address = h.address;
            sf.text    = sig;
            results.push_back(sf);
        }

        if (!hits.empty()) {
            std::cout << "[entity]   \"" << sig << "\" -> "
                      << hits.size() << " hit(s), first at 0x"
                      << std::hex << hits[0].address << std::dec << "\n";
        }

        // Bail early if thread is stopping
        if (!running.load()) break;
    }

    std::cout << "[entity] String scan complete: " << results.size()
              << " total hits\n";

    {
        std::lock_guard<std::mutex> lk(mtx);
        stringFinds = std::move(results);
        status = "String scan done (" + std::to_string(stringFinds.size()) + " hits)";
    }
}

// =====================================================================
//  Entity read: follow chain -> entity list -> read positions
// =====================================================================

void EntityReader::DoEntityRead()
{
    // 1. Follow pointer chain to reach the entity list object
    uintptr_t listAddr = FollowChain();
    if (listAddr == 0) {
        std::lock_guard<std::mutex> lk(mtx);
        status = "Chain resolved to NULL";
        entities.clear();
        return;
    }

    // 2. Read entity count from the list (ArrayList.size is an int)
    auto countOpt = ReadMemory<int32_t>(hProcess, listAddr + offsets.listSizeOffset);
    if (!countOpt) {
        std::lock_guard<std::mutex> lk(mtx);
        status = "Failed to read entity count";
        entities.clear();
        return;
    }

    int count = *countOpt;
    if (count < 0 || count > offsets.maxEntities)
        count = (count < 0) ? 0 : offsets.maxEntities;

    // 3. Read the internal array reference (ArrayList.elementData)
    uintptr_t arrayRef = ReadOop(listAddr + offsets.listArrayOffset);
    if (arrayRef == 0) {
        std::lock_guard<std::mutex> lk(mtx);
        status = "Entity array ref is NULL";
        entities.clear();
        return;
    }

    // 4. Walk the array and read each entity
    std::vector<EntityData> snapshot;
    snapshot.reserve(count);

    int refSize = oops.compressed ? 4 : 8;
    int validCount = 0;

    for (int i = 0; i < count; ++i) {
        // Array element address: arrayBase + dataOffset + i * refSize
        uintptr_t elemAddr = arrayRef + offsets.arrayDataOffset + (i * refSize);

        // Dereference to get the Entity object address
        uintptr_t entityAddr = ReadOop(elemAddr);
        if (entityAddr == 0) continue;

        EntityData ed;
        ed.index = i;

        // Read position doubles
        auto px = ReadMemory<double>(hProcess, entityAddr + offsets.posXOffset);
        auto py = ReadMemory<double>(hProcess, entityAddr + offsets.posYOffset);
        auto pz = ReadMemory<double>(hProcess, entityAddr + offsets.posZOffset);

        if (px && py && pz) {
            ed.posX = *px;
            ed.posY = *py;
            ed.posZ = *pz;

            // Sanity check: positions should be finite and within MC world bounds
            if (ed.posX > -3.0e7 && ed.posX < 3.0e7 &&
                ed.posY > -1000   && ed.posY < 1000 &&
                ed.posZ > -3.0e7 && ed.posZ < 3.0e7)
            {
                ed.valid = true;
            }
        }

        // Read bounding box (optional — follow ref to Box object)
        uintptr_t bbAddr = ReadOop(entityAddr + offsets.bbRefOffset);
        if (bbAddr != 0) {
            auto bx0 = ReadMemory<double>(hProcess, bbAddr + offsets.bbMinXOffset);
            auto by0 = ReadMemory<double>(hProcess, bbAddr + offsets.bbMinYOffset);
            auto bz0 = ReadMemory<double>(hProcess, bbAddr + offsets.bbMinZOffset);
            auto bx1 = ReadMemory<double>(hProcess, bbAddr + offsets.bbMaxXOffset);
            auto by1 = ReadMemory<double>(hProcess, bbAddr + offsets.bbMaxYOffset);
            auto bz1 = ReadMemory<double>(hProcess, bbAddr + offsets.bbMaxZOffset);

            if (bx0) ed.bbMinX = *bx0;
            if (by0) ed.bbMinY = *by0;
            if (bz0) ed.bbMinZ = *bz0;
            if (bx1) ed.bbMaxX = *bx1;
            if (by1) ed.bbMaxY = *by1;
            if (bz1) ed.bbMaxZ = *bz1;
        }

        snapshot.push_back(ed);
        if (ed.valid) ++validCount;
    }

    // 5. Console output for valid entities
    static int printCooldown = 0;
    if (++printCooldown >= 20) {  // print every ~1 second (20 * 50ms)
        printCooldown = 0;
        for (auto& e : snapshot) {
            if (e.valid) {
                printf("Entity #%d at X:%.2f Y:%.2f Z:%.2f\n",
                       e.index, e.posX, e.posY, e.posZ);
            }
        }
        if (validCount > 0)
            printf("--- %d/%d entities valid ---\n\n", validCount, count);
    }

    // 6. Publish snapshot
    {
        std::lock_guard<std::mutex> lk(mtx);
        entities = std::move(snapshot);

        char buf[128];
        snprintf(buf, sizeof(buf), "Reading %d entities (%d valid) @ 0x%llX",
                 count, validCount, static_cast<unsigned long long>(listAddr));
        status = buf;
    }
}
