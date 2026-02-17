#include "esp.h"

#include <imgui.h>
#include <cstdio>
#include <cfloat>

static constexpr float DEG2RAD = 3.14159265f / 180.0f;

// =====================================================================
//  Mat4 helpers
// =====================================================================

Mat4 Mat4::Identity()
{
    Mat4 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

Mat4 Mat4::PerspectiveFov(float fovYRad, float aspect,
                          float zNear, float zFar)
{
    float tanHalf = tanf(fovYRad * 0.5f);
    Mat4 r{};
    r.m[0][0] = 1.0f / (aspect * tanHalf);
    r.m[1][1] = 1.0f / tanHalf;
    r.m[2][2] = -(zFar + zNear) / (zFar - zNear);
    r.m[2][3] = -1.0f;
    r.m[3][2] = -(2.0f * zFar * zNear) / (zFar - zNear);
    return r;
}

Mat4 Mat4::LookAt(Vec3 eye, Vec3 target, Vec3 up)
{
    // Forward = normalize(target - eye)
    float fx = target.x - eye.x;
    float fy = target.y - eye.y;
    float fz = target.z - eye.z;
    float flen = sqrtf(fx*fx + fy*fy + fz*fz);
    if (flen < 1e-8f) flen = 1e-8f;
    fx /= flen; fy /= flen; fz /= flen;

    // Right = normalize(forward x up)
    float rx = fy * up.z - fz * up.y;
    float ry = fz * up.x - fx * up.z;
    float rz = fx * up.y - fy * up.x;
    float rlen = sqrtf(rx*rx + ry*ry + rz*rz);
    if (rlen < 1e-8f) rlen = 1e-8f;
    rx /= rlen; ry /= rlen; rz /= rlen;

    // Up = right x forward
    float ux = ry * fz - rz * fy;
    float uy = rz * fx - rx * fz;
    float uz = rx * fy - ry * fx;

    Mat4 m{};
    m.m[0][0] = rx;  m.m[1][0] = ry;  m.m[2][0] = rz;
    m.m[0][1] = ux;  m.m[1][1] = uy;  m.m[2][1] = uz;
    m.m[0][2] = -fx; m.m[1][2] = -fy; m.m[2][2] = -fz;
    m.m[3][0] = -(rx*eye.x + ry*eye.y + rz*eye.z);
    m.m[3][1] = -(ux*eye.x + uy*eye.y + uz*eye.z);
    m.m[3][2] =  (fx*eye.x + fy*eye.y + fz*eye.z);
    m.m[3][3] = 1.0f;
    return m;
}

Vec4 Mul(const Mat4& m, const Vec4& v)
{
    Vec4 r;
    r.x = m.m[0][0]*v.x + m.m[1][0]*v.y + m.m[2][0]*v.z + m.m[3][0]*v.w;
    r.y = m.m[0][1]*v.x + m.m[1][1]*v.y + m.m[2][1]*v.z + m.m[3][1]*v.w;
    r.z = m.m[0][2]*v.x + m.m[1][2]*v.y + m.m[2][2]*v.z + m.m[3][2]*v.w;
    r.w = m.m[0][3]*v.x + m.m[1][3]*v.y + m.m[2][3]*v.z + m.m[3][3]*v.w;
    return r;
}

static Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// =====================================================================
//  Build view matrix from yaw/pitch/position
// =====================================================================

static Mat4 BuildViewMatrix(const EspConfig& cfg)
{
    float yawRad   = cfg.camYaw   * DEG2RAD;
    float pitchRad = cfg.camPitch * DEG2RAD;

    // Minecraft convention: yaw 0 = -Z, yaw 90 = -X
    // Forward direction from yaw + pitch
    float fx = -sinf(yawRad) * cosf(pitchRad);
    float fy = -sinf(pitchRad);
    float fz = -cosf(yawRad) * cosf(pitchRad);

    Vec3 target = {
        cfg.camPos.x + fx,
        cfg.camPos.y + fy,
        cfg.camPos.z + fz
    };

    return Mat4::LookAt(cfg.camPos, target, {0, 1, 0});
}

// =====================================================================
//  WorldToScreen
// =====================================================================

bool WorldToScreen(const Vec3& world, const Mat4& viewProj,
                   float screenW, float screenH,
                   float& outX, float& outY)
{
    Vec4 clip = Mul(viewProj, {world.x, world.y, world.z, 1.0f});

    // Behind camera
    if (clip.w <= 0.001f) return false;

    // NDC
    float ndcX = clip.x / clip.w;
    float ndcY = clip.y / clip.w;

    // NDC → screen (flip Y: NDC +Y is up, screen +Y is down)
    outX = (ndcX * 0.5f + 0.5f) * screenW;
    outY = (-ndcY * 0.5f + 0.5f) * screenH;

    return true;
}

// =====================================================================
//  DrawEntityESP
// =====================================================================

void DrawEntityESP(const std::vector<EntityData>& entities,
                   const EspConfig& cfg,
                   float screenX, float screenY,
                   float screenW, float screenH)
{
    if (!cfg.enabled || screenW <= 0 || screenH <= 0)
        return;

    // Build view-projection matrix
    float aspect = screenW / screenH;
    Mat4 view = BuildViewMatrix(cfg);
    Mat4 proj = Mat4::PerspectiveFov(cfg.fovY * DEG2RAD, aspect,
                                      cfg.zNear, cfg.zFar);
    Mat4 viewProj = Multiply(proj, view);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImU32 boxColor = IM_COL32(
        static_cast<int>(cfg.boxR * 255),
        static_cast<int>(cfg.boxG * 255),
        static_cast<int>(cfg.boxB * 255),
        static_cast<int>(cfg.boxA * 255));

    float midX = screenX + screenW * 0.5f;
    float midY = screenY + screenH;

    for (auto& ent : entities) {
        if (!ent.valid) continue;

        // Distance check
        float dx = static_cast<float>(ent.posX) - cfg.camPos.x;
        float dy = static_cast<float>(ent.posY) - cfg.camPos.y;
        float dz = static_cast<float>(ent.posZ) - cfg.camPos.z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > cfg.maxDrawDist) continue;

        // ── Determine the 3D bounding box corners ────────────────────
        // If we have bounding box data, use it; otherwise approximate
        float minX, minY, minZ, maxX, maxY, maxZ;

        bool hasBB = (ent.bbMaxX != ent.bbMinX) ||
                     (ent.bbMaxY != ent.bbMinY) ||
                     (ent.bbMaxZ != ent.bbMinZ);
        if (hasBB) {
            minX = static_cast<float>(ent.bbMinX);
            minY = static_cast<float>(ent.bbMinY);
            minZ = static_cast<float>(ent.bbMinZ);
            maxX = static_cast<float>(ent.bbMaxX);
            maxY = static_cast<float>(ent.bbMaxY);
            maxZ = static_cast<float>(ent.bbMaxZ);
        } else {
            // Default: 0.6 x 1.8 x 0.6 entity hitbox centered at pos
            float hw = 0.3f, hh = 0.9f;
            float px = static_cast<float>(ent.posX);
            float py = static_cast<float>(ent.posY);
            float pz = static_cast<float>(ent.posZ);
            minX = px - hw; maxX = px + hw;
            minY = py;      maxY = py + hh * 2.0f;
            minZ = pz - hw; maxZ = pz + hw;
        }

        // ── Project all 8 AABB corners to 2D ─────────────────────────
        Vec3 corners[8] = {
            {minX, minY, minZ}, {maxX, minY, minZ},
            {minX, maxY, minZ}, {maxX, maxY, minZ},
            {minX, minY, maxZ}, {maxX, minY, maxZ},
            {minX, maxY, maxZ}, {maxX, maxY, maxZ},
        };

        float sMinX = FLT_MAX,  sMinY = FLT_MAX;
        float sMaxX = -FLT_MAX, sMaxY = -FLT_MAX;
        int projected = 0;

        for (auto& c : corners) {
            float sx, sy;
            if (WorldToScreen(c, viewProj, screenW, screenH, sx, sy)) {
                // Offset to overlay screen coords
                sx += screenX;
                sy += screenY;

                if (sx < sMinX) sMinX = sx;
                if (sy < sMinY) sMinY = sy;
                if (sx > sMaxX) sMaxX = sx;
                if (sy > sMaxY) sMaxY = sy;
                ++projected;
            }
        }

        // Need at least 1 corner visible
        if (projected == 0) continue;

        // Clamp to screen bounds
        float clampL = screenX;
        float clampT = screenY;
        float clampR = screenX + screenW;
        float clampB = screenY + screenH;

        if (sMinX < clampL) sMinX = clampL;
        if (sMinY < clampT) sMinY = clampT;
        if (sMaxX > clampR) sMaxX = clampR;
        if (sMaxY > clampB) sMaxY = clampB;

        float bw = sMaxX - sMinX;
        float bh = sMaxY - sMinY;
        if (bw < 2.0f || bh < 2.0f) continue;

        // ── Draw the box ─────────────────────────────────────────────
        dl->AddRect(
            ImVec2(sMinX, sMinY),
            ImVec2(sMaxX, sMaxY),
            boxColor, 0.0f, 0, cfg.thickness);

        // ── Snap lines (bottom-center of screen → top-center of box) ─
        if (cfg.showSnaplines) {
            dl->AddLine(
                ImVec2(midX, midY),
                ImVec2((sMinX + sMaxX) * 0.5f, sMaxY),
                boxColor, 1.0f);
        }

        // ── Label ────────────────────────────────────────────────────
        if (cfg.showLabels || cfg.showDistance) {
            char label[64] = {};
            if (cfg.showLabels && cfg.showDistance)
                snprintf(label, sizeof(label), "#%d [%.0fm]",
                         ent.index, dist);
            else if (cfg.showLabels)
                snprintf(label, sizeof(label), "#%d", ent.index);
            else
                snprintf(label, sizeof(label), "%.0fm", dist);

            dl->AddText(ImVec2(sMinX, sMinY - 14.0f), boxColor, label);
        }
    }
}
