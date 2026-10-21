# D7VK: present the frame before reading it back

`present-before-download.patch` (against D7VK 3.1.1, `src/ddraw/ddraw7/ddraw7_surface.cpp`).

**What it does.** The game presents each frame with `BltFast(primary <- the 3D frame)`. `DDraw7Surface::BltFast` (and `Blt`)
first *downloaded* the source if its D3D9 side had been drawn on, a GPU -> CPU read of the whole frame (8 MB at 1080p) that also
makes the CPU wait until the GPU has finished the frame, and only then took the windowed shortcut that just calls `Present`. The
patch moves the shortcut in front of the download, when the frame's DDraw-side copy holds no pending changes (nothing was Blt or
Lock'ed onto it since the last upload; with the mod's `UI_QUAD_BLIT=true` the game's interface no longer is), so the frame is
presented without any copy and without waiting for the GPU. In every other case the code runs as before.

**Measured (2026-10-21, test rig).** The memory copy of the frame (about 0.8 ms of the main thread per frame at 1080p)
disappears from the profile and the picture is unchanged (screenshots). The rig's own display path (Xvfb) is slower than the game, so
it cannot show the frame-rate gain; that needs a run on a real display (um.dll profiler: the BltFast line).

**Measured on a real-GPU path (2026-10-21, gamescope headless on the NVIDIA card, the multiplayer base, um.dll profiler).** Main thread per frame, with um-engine's
lighting kernel on: **11.8 ms (84 FPS) with the stock D7VK, 6.9 - 7.4 ms (135 - 145 FPS) with this patch.** `BltFast` 4.46 ms -> 0.01 ms: the stock build waited
3.8 ms for the GPU in the download and copied the frame (0.7 ms). Screenshots of the same scene differ only where things move (idle characters, sparks, water, cursor).

**Ready-made build: `Universal-Mod/d7vk/ddraw.dll` (8.9 MB, stripped, win32; D7VK 3.1.1 + `present-before-download.patch`, built with the submodule branch heads of 2026-10-21, not the pinned commits).**
um.dll 1.4.12 loads it when um.cfg says `DDRAW_DLL="d7vk\ddraw.dll"` (a path relative to the mod folder, or absolute): the game folder is not touched, its own `ddraw.dll` stays loaded but unused
(um.log: "DDRAW_DLL: DirectDraw comes from ..."). Without um.dll, copy it over `Engine/ddraw.dll` (keep the old one; the stock file is D7VK 3.1.1).
Undo = empty `DDRAW_DLL` (or put the old file back). Needs `UI_QUAD_BLIT=true` in um.cfg (the default) so that the interface does not mark the frame as changed.

**Build (what was done here).** meson 1.12 (`pip install meson`), the Vulkan-Headers and SPIRV-Headers checkouts in `include/vulkan`
and `include/spirv`, `subprojects/dxbc-spirv` (with its `submodules/spirv_headers`) and `subprojects/libdisplay-info` (branch
`windows`) cloned, glslang's release binary in PATH, then

    meson setup --cross-file build-win32.txt --buildtype release -Denable_dxgi=false -Denable_d3d8=false \
          -Denable_d3d10=false -Denable_d3d11=false build32 && ninja -C build32 && i686-w64-mingw32-strip build32/src/ddraw/ddraw.dll

(the `vcs_tag` in `meson.build` needs `fallback: 'd7vk-local'` without a git checkout). The result is the game's 32-bit `ddraw.dll`
(8.9 MB). The submodule commits were the branch heads of 2026-10-21, not the ones D7VK 3.1.1 pins: use the pinned ones for a release.
