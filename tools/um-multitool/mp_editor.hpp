// File Processing > MP: the multiplayer characters (<game>/mp/N.mp, mp_file.hpp), listed and edited.
#pragma once

#include <string>

struct Library;

namespace mpedit {
void DrawTab(Library& lib); // inside the MP sub-tab
void OpenFolder(Library& lib, const std::string& folder); // shows that folder (gui --mp)
} // namespace mpedit
