#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

// ── Entity position + bounding box read from JVM heap ────────────────
struct EntityData {
    int    index = 0;
    double posX = 0, posY = 0, posZ = 0;
    double bbMinX = 0, bbMinY = 0, bbMinZ = 0;
    double bbMaxX = 0, bbMaxY = 0, bbMaxZ = 0;
    bool   valid = false;
};

// ── JVM string found during class-name scan ──────────────────────────
struct StringFind {
    uintptr_t   address = 0;
    std::string text;
};

// ── JVM Compressed Oops configuration ────────────────────────────────
// HotSpot x64 with <32 GB heap: ref is 4 bytes, real_addr = ref << 3.
// With >32 GB or certain flags: ref is 8 bytes, no shift.
struct OopConfig {
    bool      compressed = true;   // true = 4-byte refs shifted
    int       shift      = 3;      // usually 3
    uintptr_t heapBase   = 0;      // usually 0
};

// ── Offsets for reading entity data from JVM objects ─────────────────
// All values are byte offsets within the respective Java objects.
// These MUST be discovered per-version (Cheat Engine / experimentation).
// Defaults are placeholder starting points for 1.21.x HotSpot x64.
struct EntityOffsets {
    // ── Pointer chain: base -> [off0] -> [off1] -> ... -> entity list ─
    // chainBase is an absolute address (e.g., MinecraftClient static).
    // Each chainOffset is dereferenced as an oop, adding the next offset.
    uintptr_t          chainBase    = 0;
    std::vector<int>   chainOffsets;        // e.g. {0x10, 0x48, 0x20}

    // ── Entity list (Java ArrayList or similar) ──────────────────────
    int listSizeOffset  = 0x10;   // ArrayList.size     (int, 4 bytes)
    int listArrayOffset = 0x14;   // ArrayList.elementData (oop ref)

    // ── Java Object[] array ──────────────────────────────────────────
    // Header: mark(8) + klass(4) + length(4) = 16 bytes, data at +16
    int arrayDataOffset = 0x10;

    // ── Entity object field offsets ──────────────────────────────────
    // Minecraft Entity stores position as 3 doubles.
    // In 1.21.x (Yarn): Entity.pos is a Vec3d, but the JVM may inline
    // or the fields may be directly on Entity. Discover via CE.
    int posXOffset = 0x98;        // double Entity.x  (placeholder)
    int posYOffset = 0xA0;        // double Entity.y
    int posZOffset = 0xA8;        // double Entity.z

    // Bounding box is a separate object ref on Entity.
    int bbRefOffset  = 0xB0;      // oop Entity.boundingBox -> Box
    int bbMinXOffset = 0x10;      // double Box.minX  (after 12-byte header + pad)
    int bbMinYOffset = 0x18;      // double Box.minY
    int bbMinZOffset = 0x20;      // double Box.minZ
    int bbMaxXOffset = 0x28;      // double Box.maxX
    int bbMaxYOffset = 0x30;      // double Box.maxY
    int bbMaxZOffset = 0x38;      // double Box.maxZ

    // Maximum entities to read (safety cap)
    int maxEntities = 256;
};

// =====================================================================
//  EntityReader — background-threaded JVM entity data reader
// =====================================================================
class EntityReader {
public:
    EntityReader() = default;
    ~EntityReader();

    // Start the background read loop targeting `process`.
    void Start(HANDLE process);

    // Stop the background thread.
    void Stop();

    bool IsRunning() const { return running.load(); }

    // ── Thread-safe accessors ────────────────────────────────────────

    // Latest entity snapshot.
    std::vector<EntityData> GetEntities() const;

    // String-scan results (class name discovery).
    std::vector<StringFind> GetStringFinds() const;

    // Status message for display.
    std::string GetStatus() const;

    // ── Commands (set flags, worker picks them up) ───────────────────

    // Request a one-shot string scan for JVM class names.
    void RequestStringScan();

    // ── Configuration (set before Start, or while running) ───────────

    OopConfig      oops;
    EntityOffsets  offsets;

    // Interval between entity reads (ms).
    int readIntervalMs = 50;

    // Whether the read loop is actively reading entities.
    // If false, the thread idles (useful to pause without stopping).
    std::atomic<bool> entityReadEnabled{ false };

private:
    void WorkerLoop();

    // One-shot: scan all readable memory for known MC class name strings.
    void DoStringScan();

    // Continuous: follow pointer chain, read entity list, populate snapshot.
    void DoEntityRead();

    // Dereference a JVM oop (compressed or raw) at `addr`.
    uintptr_t ReadOop(uintptr_t addr) const;

    // Follow the configured pointer chain from chainBase through offsets.
    uintptr_t FollowChain() const;

    HANDLE          hProcess = nullptr;
    std::thread     worker;
    std::atomic<bool> running{ false };
    std::atomic<bool> stringScanRequested{ false };

    mutable std::mutex mtx;
    std::vector<EntityData> entities;
    std::vector<StringFind> stringFinds;
    std::string             status = "idle";
};
