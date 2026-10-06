#include "texedit/texture_app.hpp"

#include "i18n.hpp"
#include "imgui.h"
#include "viewer/library.hpp"

namespace texedit {

void DrawTab(Library&) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", i18n::Tr("Texture Editor: coming soon. Planned: open any game texture (.mmp, .dds, .png), a pattern generator, recoloring, "
                                      "smart area selection, and a quest map creator."));
}

} // namespace texedit
