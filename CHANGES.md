● Build successful — WD42.exe (660 KB). Here's what was added:

  New: entity.h/cpp — Background-Threaded JVM Entity Reader

  JVM String Scanner (Scan Strings button):
  - Scans all readable memory for 8 known Minecraft class-name signatures as raw UTF-8 bytes (net/minecraft/client/MinecraftClient, net/minecraft/entity/Entity,   
  etc.)
  - Reports addresses and hit counts — proves you can locate JVM class metadata in the heap

  Compressed Oops Support (OopConfig):
  - Configurable shift (default 3) and heap base (default 0)
  - Handles both 4-byte compressed and 8-byte raw JVM references
  - Toggle in the ImGui panel

  Pointer Chain Follower (FollowChain()):
  - Input a base address + comma-separated hex offsets (e.g. 0x10,0x48,0x20)
  - Follows base → [deref+off0] → [deref+off1] → ... → entity list
  - Offsets discovered via Cheat Engine pointer scan

  Entity List Reader (DoEntityRead()):
  - Reads entity count from the list (ArrayList.size offset)
  - Dereferences the internal array (ArrayList.elementData)
  - Walks the array reading each entity's oop reference
  - Reads posX/posY/posZ as doubles at configurable offsets
  - Reads bounding box (6 doubles) through a separate oop dereference
  - Sanity checks positions against MC world bounds

  Background Thread:
  - Start/Stop controls in ImGui
  - Configurable interval (10–500ms slider)
  - Mutex-protected snapshot — main thread reads safely each frame
  - Console output: Entity #N at X:%.2f Y:%.2f Z:%.2f every ~1 second

  ImGui Tabbed Interface:
  - Entities tab: JVM config, pointer chain, field offsets, entity list with hover tooltips for bounding boxes
  - Scanner tab: AOB pattern scanner (unchanged)
  - Memory tab: hex reader (unchanged)

  File rename: process.h/cpp → mc_process.h/cpp to avoid shadowing MSVC's CRT <process.h> (needed by <thread>).

● Build successful — WD42.exe (705 KB). Here's what was added:

  New: esp.h/cpp — 3D→2D ESP Box Renderer

  Math primitives (no external lib):
  - Vec3, Vec4, Mat4 with identity, perspective, and LookAt constructors
  - Matrix multiply, vector transform

  Projection pipeline:
  - BuildViewMatrix() — constructs a view matrix from camera position + yaw/pitch using Minecraft's coordinate convention (yaw 0 = -Z, yaw 90 = -X)
  - Mat4::PerspectiveFov() — standard perspective projection
  - WorldToScreen() — full world→clip→NDC→screen transform, returns false for behind-camera points

  ESP drawing (DrawEntityESP()):
  - For each valid entity: computes distance, culls beyond maxDrawDist
  - Builds 3D AABB from bounding box data (or falls back to default 0.6x1.8x0.6 hitbox)
  - Projects all 8 AABB corners to 2D, finds the enclosing screen rectangle
  - Clamps to screen bounds, skips tiny boxes (<2px)
  - Draws via ImGui::GetBackgroundDrawList()->AddRect() — renders behind ImGui windows
  - Optional: snap lines (bottom-center screen → box), #index [distance] labels

  ImGui config (in Entities tab):
  - Enable/disable, label/distance/snapline toggles
  - Color picker, thickness slider, max distance slider
  - Camera position/yaw/pitch/FOV controls (placeholder — user sets from F3 screen or memory reads)

  Hotkeys:
  - F3 — toggle ESP on/off (edge-triggered)
  - Footer updated: INSERT = click-through | F3 = ESP | ESC = quit

  Important note: The camera settings default to identity-ish (position 0,70,0 looking at -Z). For boxes to actually align with the game view, you'd need to read  
  the player's position and rotation from memory and feed them into espCfg.camPos/camYaw/camPitch. The projection math is complete and correct — it just needs real
   camera data.