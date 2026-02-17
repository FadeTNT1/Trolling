#pragma once

#include "entity.h"

#include <cstdint>
#include <cmath>
#include <vector>

// ── Minimal math types (no external lib needed) ──────────────────────
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };

struct Mat4 {
    float m[4][4] = {};

    static Mat4 Identity();
    static Mat4 PerspectiveFov(float fovYRad, float aspect, float zNear, float zFar);
    static Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up);
};

Vec4 Mul(const Mat4& m, const Vec4& v);

// ── ESP configuration ────────────────────────────────────────────────
struct EspConfig {
    bool enabled = true;

    // Box style
    float boxR = 1.0f, boxG = 0.15f, boxB = 0.15f, boxA = 1.0f;
    float thickness = 2.0f;
    bool  showLabels = true;
    bool  showDistance = true;
    bool  showSnaplines = false;

    // Camera — user must supply these from memory reads or defaults.
    // With all-zero rotation, the camera faces -Z (OpenGL convention).
    Vec3  camPos   = { 0, 70, 0 };    // world position
    float camYaw   = 0;                // degrees, 0 = -Z
    float camPitch = 0;                // degrees, 0 = horizontal

    // Projection
    float fovY     = 70.0f;           // vertical FOV in degrees
    float zNear    = 0.05f;
    float zFar     = 1000.0f;

    // Culling
    float maxDrawDist = 256.0f;        // don't draw beyond this
};

// ── ESP drawing ──────────────────────────────────────────────────────

// Project a world-space point to screen-space pixel coordinates.
// Returns false if the point is behind the camera.
bool WorldToScreen(const Vec3& world, const Mat4& viewProj,
                   float screenW, float screenH,
                   float& outX, float& outY);

// Draw ESP boxes for all valid entities onto ImGui's background draw list.
// `screenOrigin` is the top-left of the Minecraft window in screen coords.
// `screenW`/`screenH` is the Minecraft window size.
void DrawEntityESP(const std::vector<EntityData>& entities,
                   const EspConfig& cfg,
                   float screenX, float screenY,
                   float screenW, float screenH);
