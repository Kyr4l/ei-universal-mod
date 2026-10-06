// The Texture Editor: um-multitool's main tab for the game's textures (see _cpr/PENDING.md #73 / #93).
// For now only the tab itself; the tools come later.
#pragma once

struct Library;

namespace texedit {

// Inside the ImGui frame, in the tab's content region.
void DrawTab(Library& lib);

} // namespace texedit
