# D7VK: sun shadows for the game's pre-transformed world (PENDING #141)

The new and changed sources (D7VK 3.1.1 tree + `../present-before-download.patch`; copy them over the same paths of the checkout, then build as in `../README.md`).
Written 2026-10-24; docs/shadows.md ("The replacement") explains what the stock game does and why this design.

**New:** `src/dxvk/dxvk_um_shadow.{h,cpp}` (pipelines, samplers, the shadow map image), `src/dxvk/shaders/dxvk_um_shadow_*` (depth pass vertex / fragment, resolve in
three variants: single sample, multisampled interior pixels, multisampled object edges).
**Changed:** `dxvk_context.{h,cpp}` (`umShadowBegin / Capture / End`: the passes), `dxvk_objects.h`, `dxvk/meson.build`, `d3d9_device.{h,cpp}` (captures every depth-writing
`DrawIndexedPrimitiveUP` while a world pass is open; the exported entry points; an unclosed pass is dropped at `EndScene`), `ddraw_main.cpp` + `ddraw.def` (the exports
`UmShadowBegin()`, `UmShadowCaster(int)` (the next draws are / are not casters: the game's per-kind shadow switches), `UmShadowGuard(float, float)` (the pixels the game's world coordinates are shifted by, its guard band: the fixed-function vertex shader of the pre-transformed vertices subtracts them) `UmShadowEnd(const DxvkUmShadowParams*)`, `UmShadowScreenshot(path, half)` (the colour buffer as a PNG, after `UmShadowEnd`, on the game's thread: the frame recorder's picture) and `UmShadowStats(uint32_t[4])` (frames closed, caster draws of the last one, its result code, map size); the struct is in `dxvk_um_shadow.h` and in um-engine's `world_shadow.cpp`).

**How it works.** The game sends PRE-TRANSFORMED vertices (x, y in pixels, z, rhw). um-engine calls `UmShadowBegin` before the terrain is drawn and `UmShadowEnd` after the objects, with
the sun, the ambient colour and the inverse of the pipeline's screen mapping. In between D3D9DeviceEx keeps each opaque draw (its slice of the UP buffer, the alpha test and
the texture) in a list. At the end one pass draws the list into a 2048 x 2048 depth map seen from the sun (the vertex shader turns the screen-space vertices back into world space with
one matrix, so no CPU work and every alpha-tested texture cuts its own shadow), then a full-screen pass reads the game's depth buffer, rebuilds each pixel's world position and multiplies the
colour by the ratio of the game's own lighting (`max(ambient, sun)` -> `ambient`) where the sun does not reach (5 x 5 distance-weighted hardware PCF). Terrain, objects and units are all casters and receivers.
Multisampled targets (the mod's MSAA) are shaded once per pixel and once per sample on object edges.

**Measured (2026-10-24, gamescope on the GTX 1660 SUPER, the multiplayer base, noon, um.dll profiler):** main thread 8.5 ms per frame with the game's own shadows, 4.9 ms with these.
