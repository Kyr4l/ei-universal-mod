// The Settings tab's sources: where figures, textures, texts and the items database come from, for
// every tab of the GUI (3D Viewer, Map Editor). Views watch lib.figuresVersion / texturesVersion.
#pragma once

#include <GLFW/glfw3.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>

#include "alerts.hpp"
#include "library.hpp"
#include "ui_common.hpp"

namespace ui {

struct SourcesState {
    char figurePath[1024] = "";
    char texturePath[1024] = "";
    char textPath[1024] = "";
    char mapPath[1024] = "";
    char lightingPath[1024] = "";
    char questPath[1024] = "";
    char questPackPath[1024] = "";
    char textPackLanguage[32] = "";
    char textPackPath[1024] = "";
    int capturingKey = -1; // the map key being rebound (waiting for a key press)
    char databasePath[1024] = "";
    std::string message;
};

// Keys that print a character (the only ones glfwGetKeyName may be asked about: asking about others logs
// "Invalid scancode" on Wayland).
inline bool IsPrintableKey(int key) {
    return key == 39 || (key >= 44 && key <= 57) || key == 59 || key == 61 || (key >= 65 && key <= 93) || key == 96 || key == 161 || key == 162;
}

// A key (GLFW code = a key position) by what the current keyboard layout prints on it: the key at the
// US "W" position reads "Z" on an AZERTY keyboard.
inline std::string KeyName(int key) {
    if (const char* name = IsPrintableKey(key) ? glfwGetKeyName(key, 0) : nullptr) {
        std::string s = name;
        if (s.size() == 1) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        return s;
    }
    switch (key) {
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
    case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
    case GLFW_KEY_LEFT_CONTROL: return "Left Ctrl";
    case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
    case GLFW_KEY_LEFT_ALT: return "Left Alt";
    case GLFW_KEY_RIGHT_ALT: return "Right Alt";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_UP: return "Up";
    case GLFW_KEY_DOWN: return "Down";
    case GLFW_KEY_LEFT: return "Left";
    case GLFW_KEY_RIGHT: return "Right";
    case GLFW_KEY_PAGE_UP: return "Page Up";
    case GLFW_KEY_PAGE_DOWN: return "Page Down";
    case GLFW_KEY_HOME: return "Home";
    case GLFW_KEY_END: return "End";
    case GLFW_KEY_INSERT: return "Insert";
    case GLFW_KEY_DELETE: return "Delete";
    case GLFW_KEY_BACKSPACE: return "Backspace";
    case GLFW_KEY_ENTER: return "Enter";
    default: break;
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return "Keypad " + std::to_string(key - GLFW_KEY_KP_0);
    return "Key " + std::to_string(key);
}

inline bool IsModifierKey(int key) { return key >= GLFW_KEY_LEFT_SHIFT && key <= GLFW_KEY_RIGHT_SUPER; }

inline int HeldMods(GLFWwindow* w) {
    auto down = [&](int k) { return glfwGetKey(w, k) == GLFW_PRESS; };
    return (down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL) ? config::kModCtrl : 0) |
           (down(GLFW_KEY_LEFT_SHIFT) || down(GLFW_KEY_RIGHT_SHIFT) ? config::kModShift : 0) |
           (down(GLFW_KEY_LEFT_ALT) || down(GLFW_KEY_RIGHT_ALT) ? config::kModAlt : 0);
}

inline std::string BindName(const config::KeyBind& b) {
    std::string s;
    if (b.mods & config::kModCtrl) s += "Ctrl+";
    if (b.mods & config::kModShift) s += "Shift+";
    if (b.mods & config::kModAlt) s += "Alt+";
    if (b.mods & config::kModLetter) return s + std::string(1, static_cast<char>(b.key)); // the letter itself
    return s + KeyName(b.key);
}

// The key that prints this letter on the current layout (GLFW key code), or 0.
inline int KeyForLetter(int letter) {
    for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_GRAVE_ACCENT; ++key) {
        if (!IsPrintableKey(key)) continue;
        const char* name = glfwGetKeyName(key, 0);
        if (name && name[0] && !name[1] && std::toupper(static_cast<unsigned char>(name[0])) == letter) return key;
    }
    return 0;
}

// The letter a key prints on the current layout (upper case), or 0.
inline int LetterOfKey(int key) {
    const char* name = IsPrintableKey(key) ? glfwGetKeyName(key, 0) : nullptr;
    if (!name || !name[0] || name[1] || !std::isalpha(static_cast<unsigned char>(name[0]))) return 0;
    return std::toupper(static_cast<unsigned char>(name[0]));
}

// The Map Editor's keys: click one, then press the key (with its modifiers, for actions) to use.
// Escape cancels.
inline void KeyBindings(Library& lib, SourcesState& st) {
    if (st.capturingKey >= 0) {
        GLFWwindow* window = glfwGetCurrentContext();
        const bool action = st.capturingKey >= config::kFirstActionKey;
        for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST && window; ++key) {
            if (glfwGetKey(window, key) != GLFW_PRESS) continue;
            if (action && IsModifierKey(key)) continue; // wait for the key that goes with them
            if (key != GLFW_KEY_ESCAPE) {
                // A letter bind (Ctrl+A) stays one: it records the letter pressed, not the key's position.
                const bool byLetter = (lib.mapKeys[st.capturingKey].mods & config::kModLetter) && LetterOfKey(key);
                lib.mapKeys[st.capturingKey] = byLetter ? config::KeyBind{LetterOfKey(key), HeldMods(window) | config::kModLetter}
                                                        : config::KeyBind{key, action ? HeldMods(window) : 0};
                lib.SaveConfig();
            }
            st.capturingKey = -1;
            break;
        }
    }
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (int k = 0; k < config::kMapKeyCount; ++k) {
            if (k == config::kFirstActionKey) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Spacing();
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(config::MapKeyLabel(k));
            ImGui::TableNextColumn();
            std::string label = st.capturingKey == k ? "press a key... (Escape: cancel)" : BindName(lib.mapKeys[k]);
            bool twice = false;
            for (int j = 0; j < config::kMapKeyCount; ++j) if (j != k && lib.mapKeys[j] == lib.mapKeys[k]) twice = true;
            if (twice) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.6f, 0.35f, 1));
            if (ImGui::Button((label + "##key" + std::to_string(k)).c_str(), ImVec2(260, 0))) st.capturingKey = k;
            if (twice) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(twice ? "This key is used twice" : "Click, then press the key to use");
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Defaults##keys")) {
        lib.mapKeys = config::DefaultMapKeys();
        lib.SaveConfig();
        st.capturingKey = -1;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Movement at the W A S D, E (up), Q (down) and Left Shift positions of a US keyboard\n"
                          "(Z Q S D, E, A on AZERTY): the same hand position on every layout.\n"
                          "Ctrl+Tab logic mode, Ctrl+T next map, U unload the last file, Ctrl+R reset the camera, Ctrl+L lighting,\n"
                          "Ctrl+S save, Ctrl+Z / Ctrl+Y undo / redo, G move, R rotate, T scale (then X/Y/Z), Ctrl+F find, Ctrl+A select all, Delete,\n"
                          "Ctrl+C / Ctrl+V copy / paste, Ctrl+D duplicate (Ctrl+A/C/V/D by letter).");
}

// A grey explanation that wraps at the column's edge.
inline void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A list of files or folders in priority order: shown highest first (like the layered sources), stored
// lowest first so that a later entry wins. Up/Down reorder, X removes. True when changed.
inline bool PathList(const char* id, std::vector<std::string>& paths, char* input, size_t inputSize, const char* hint, const char* empty) {
    bool changed = false;
    ImGui::PushID(id);
    int removeAt = -1, upAt = -1, downAt = -1;
    for (size_t i = paths.size(); i-- > 0;) {
        ImGui::PushID(static_cast<int>(i));
        std::error_code ec;
        float rowWidth = ImGui::GetContentRegionAvail().x;
        const float buttons = 118.0f;
        StatusDot(std::filesystem::exists(paths[i], ec), std::filesystem::is_directory(paths[i], ec) ? "folder" : "file", "not found");
        ImGui::SameLine();
        ImGui::BeginChild("##p", ImVec2(std::max(rowWidth - buttons - ImGui::GetCursorPosX(), 20.0f), ImGui::GetTextLineHeight()),
                          false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted(paths[i].c_str());
        ImGui::EndChild();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", paths[i].c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(rowWidth - buttons);
        ImGui::BeginDisabled(i + 1 == paths.size());
        if (ImGui::SmallButton("Up")) upAt = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Down")) downAt = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeAt = static_cast<int>(i);
        ImGui::PopID();
    }
    if (upAt >= 0) { std::swap(paths[upAt], paths[upAt + 1]); changed = true; }
    else if (downAt > 0) { std::swap(paths[downAt], paths[downAt - 1]); changed = true; }
    else if (removeAt >= 0) { paths.erase(paths.begin() + removeAt); changed = true; }
    if (paths.empty()) ImGui::TextDisabled("%s", empty);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##add", hint, input, inputSize);
    std::string picked;
    if (ImGui::Button("File...") && PickFile(picked)) std::snprintf(input, inputSize, "%s", picked.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Folder...") && PickFolder(picked)) std::snprintf(input, inputSize, "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(input[0] == '\0');
    if (ImGui::Button("Add")) { // new entries go on top: the highest priority
        paths.push_back(input);
        input[0] = '\0';
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

// The text language packs: (language, path) rows, and a row to add one.
inline bool TextPackList(Library& lib, SourcesState& st) {
    bool changed = false;
    ImGui::PushID("textpacks");
    int removeAt = -1;
    if (ImGui::BeginTable("##packs", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("lang", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, 24);
        for (size_t i = 0; i < lib.textPacks.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::error_code ec;
            StatusDot(std::filesystem::exists(lib.textPacks[i].second, ec), "", "not found");
            ImGui::SameLine();
            ImGui::TextUnformatted(lib.textPacks[i].first.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(lib.textPacks[i].second.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lib.textPacks[i].second.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("X")) removeAt = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeAt >= 0) { lib.textPacks.erase(lib.textPacks.begin() + removeAt); changed = true; }
    if (lib.textPacks.empty()) ImGui::TextDisabled("(no language packs)");
    ImGui::SetNextItemWidth(60);
    ImGui::InputTextWithHint("##lang", "eng", st.textPackLanguage, sizeof(st.textPackLanguage));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##path", "path to a folder of text files or a texts*.res", st.textPackPath, sizeof(st.textPackPath));
    std::string picked;
    if (ImGui::Button("File...") && PickFile(picked)) std::snprintf(st.textPackPath, sizeof(st.textPackPath), "%s", picked.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Folder...") && PickFolder(picked)) std::snprintf(st.textPackPath, sizeof(st.textPackPath), "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(st.textPackLanguage[0] == '\0' || st.textPackPath[0] == '\0' || std::strchr(st.textPackLanguage, '='));
    if (ImGui::Button("Add")) {
        lib.textPacks.push_back({st.textPackLanguage, st.textPackPath});
        st.textPackPath[0] = '\0'; // the language stays: its next folder is often next
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

// One layered source list: highest priority on top, with Up/Down/Remove, and an add row.
// Returns true when the list changed.
inline bool LayerList(const char* id, LayeredAssetSource& source, char* path, size_t pathSize, std::string& message,
                      const char* hint) {
    bool changed = false;
    ImGui::PushID(id);
    for (size_t i = source.layers.size(); i-- > 0;) {
        auto& layer = source.layers[i];
        ImGui::PushID(static_cast<int>(i));
        float rowWidth = ImGui::GetContentRegionAvail().x;
        const float buttons = 118.0f;
        StatusDot(layer.ok, layer.source.isArchive ? "RES archive" : "folder", layer.error);
        ImGui::SameLine();
        ImGui::BeginChild("##p", ImVec2(std::max(rowWidth - buttons - ImGui::GetCursorPosX(), 20.0f), ImGui::GetTextLineHeight()),
                          false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted(layer.path.c_str());
        ImGui::EndChild();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", layer.path.c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(rowWidth - buttons);
        ImGui::BeginDisabled(i + 1 == source.layers.size());
        if (ImGui::SmallButton("Up")) { source.MoveLayerUp(i); changed = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Down")) { source.MoveLayerDown(i); changed = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) { source.RemoveLayer(i); changed = true; }
        ImGui::PopID();
        if (changed) break;
    }
    if (source.layers.empty()) ImGui::TextDisabled("%s", hint);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##add", "path to a .res archive or a folder", path, pathSize);
    std::string picked;
    if (ImGui::Button("File...") && PickFile(picked)) std::snprintf(path, pathSize, "%s", picked.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Folder...") && PickFolder(picked)) std::snprintf(path, pathSize, "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(path[0] == '\0');
    if (ImGui::Button("Add")) {
        bool ok = source.AddLayer(path);
        message = ok ? std::string("Added ") + path : "Could not add " + std::string(path) + ": " + source.layers.back().error;
        path[0] = '\0';
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

inline void SourcesTab(Library& lib, SourcesState& st) {
    ImGui::TextWrapped("Sources are searched top to bottom: add the base game first, then mods on top (a mod's file "
                       "overrides the game's). Each source is a .res archive or a folder of loose files.");
    ImGui::Spacing();

    ImGui::SeparatorText("Figures");
    Hint("figures.res, or a folder of .fig/.bon/.lnk/.mod files");
    if (LayerList("fig", lib.figures, st.figurePath, sizeof(st.figurePath), st.message, "(none yet)")) {
        lib.RebuildFigureIndex();
        lib.SaveConfig();
    }

    ImGui::SeparatorText("Textures");
    Hint("textures.res and redress.res (and a mod's map textures, e.g. textures-zones.res), or a folder of .mmp or .dds files");
    if (LayerList("tex", lib.textures, st.texturePath, sizeof(st.texturePath), st.message, "(none yet)")) {
        lib.RebuildTextureIndex();
        lib.SaveConfig();
    }

    ImGui::SeparatorText("Texts");
    Hint("texts.res and textslmp.res, or a folder of loose text files (item names and descriptions)");
    if (LayerList("txt", lib.texts, st.textPath, sizeof(st.textPath), st.message, "(none: items show no name or description)")) {
        ++lib.version;
        lib.SaveConfig();
    }
    Hint("language packs: the same texts in different languages, for File Processing > Texts (e.g. eng: res-texts/texts-eng_res");
    Hint("and res-texts/textslmp-eng_res, fra: ...). A language can have several folders or .res archives.");
    if (TextPackList(lib, st)) {
        ++lib.textPacksVersion;
        lib.SaveConfig();
    }

    ImGui::SeparatorText("Maps");
    Hint("folders of .mpr and .mob files (the game's maps folder, then a mod's): the Map Editor lists them");
    if (LayerList("map", lib.maps, st.mapPath, sizeof(st.mapPath), st.message, "(none: open maps by path in the Map Editor)")) {
        ++lib.mapsVersion;
        lib.SaveConfig();
    }

    ImGui::SeparatorText("Quests");
    Hint("folders of .mq files or unpacked quests (<name>_mq/<name>/map.txt): different quests, a later folder replacing the same quest");
    if (PathList("quest", lib.questFolders, st.questPath, sizeof(st.questPath), "path to a folder of .mq files or unpacked quests",
                 "(none)"))
        lib.SaveConfig();
    Hint("language packs: folders holding the SAME quests in different languages (e.g. Universal-Mod/lang-packs/eng/maps, fra/maps, kor/maps).");
    Hint("Their quests come before the folders above; changes that do not depend on the language (areas, map.txt, quest.ini) go to every pack.");
    if (PathList("questpack", lib.questPacks, st.questPackPath, sizeof(st.questPackPath), "path to a language pack's quest folder",
                 "(none)"))
        lib.SaveConfig();

    ImGui::SeparatorText("Lighting");
    Hint("the game's config/lights*.ini (sun, ambient and sky colours per hour), or folders holding them: the Map Editor's lighting.");
    Hint("Top first: a lights*.ini found higher up replaces one of the same name below (a mod's over the game's).");
    if (PathList("light", lib.lightingFiles, st.lightingPath, sizeof(st.lightingPath), "path to a lights*.ini or a folder (e.g. the game's config)",
                 "(none: the Map Editor lights the map with a neutral sun)"))
        lib.SaveConfig();

    ImGui::SeparatorText("Database");
    Hint("database.res or databaselmp.res (whichever holds items.idb), or its spreadsheet (.xlsx, .ods), compiled when loaded");
    StatusDot(lib.dbLoaded, lib.dbLoaded ? lib.dbPath : "", lib.dbError);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##db", "path to databaselmp.res / .xlsx / .ods", st.databasePath, sizeof(st.databasePath));
    std::string picked;
    if (ImGui::Button("File...##db") && PickFile(picked)) std::snprintf(st.databasePath, sizeof(st.databasePath), "%s", picked.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(st.databasePath[0] == '\0');
    if (ImGui::Button("Load##db")) {
        if (lib.LoadDatabase(st.databasePath)) {
            st.message = "Loaded " + lib.db.sourceLabel + ": " + std::to_string(lib.db.materials.size()) + " materials, " +
                         std::to_string(lib.db.List(items::Category::Weapons).size()) + " weapons, " +
                         std::to_string(lib.db.List(items::Category::Armors).size()) + " armors, " +
                         std::to_string(lib.db.List(items::Category::QuickItems).size()) + " quick items, " +
                         std::to_string(lib.db.List(items::Category::QuestItems).size()) + " quest items, " +
                         std::to_string(lib.db.List(items::Category::LootItems).size()) + " loot items";
            lib.SaveConfig();
        } else {
            st.message = "Database not loaded: " + lib.dbError;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(lib.dbPath.empty());
    if (ImGui::Button("Reload")) {
        st.message = lib.LoadDatabase(lib.dbPath) ? "Reloaded " + lib.db.sourceLabel : "Database not loaded: " + lib.dbError;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the same file again, after editing it elsewhere");
    if (!lib.dbLoaded && !lib.dbError.empty()) Note(lib.dbError);

    ImGui::SeparatorText("Layout");
    if (ImGui::Checkbox("3D Viewer: item list on the right of the view", &lib.viewerSidebarRight)) lib.SaveConfig();
    if (ImGui::Checkbox("Map Editor: panel on the right of the view", &lib.mapSidebarRight)) lib.SaveConfig();

    ImGui::SeparatorText("Background");
    {
        // A picture behind the menus: for every tab, or per main tab (which wins).
        static const char* const labels[6] = {"All tabs", "File Processing", "3D Viewer", "Map Editor", "Settings", "UM DLL Connector"};
        for (int i = 0; i < 6; ++i) {
            std::string& path = i == 0 ? lib.background : lib.tabBackground[i - 1];
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(labels[i]);
            ImGui::SameLine(140);
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "%s", path.c_str());
            ImGui::SetNextItemWidth(-110);
            if (ImGui::InputTextWithHint("##bg", i == 0 ? "none" : "the one for all tabs", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                path = buf;
                lib.SaveConfig();
            }
            ImGui::SameLine();
            std::string picked;
            if (ImGui::Button("File...") && PickFile(picked)) { path = picked; lib.SaveConfig(); }
            ImGui::SameLine();
            ImGui::BeginDisabled(path.empty());
            if (ImGui::Button("X")) { path.clear(); lib.SaveConfig(); }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderFloat("Opacity##bg", &lib.backgroundOpacity, 0.0f, 1.0f, "%.2f")) lib.backgroundOpacity = std::min(std::max(lib.backgroundOpacity, 0.0f), 1.0f);
        if (ImGui::IsItemDeactivatedAfterEdit()) lib.SaveConfig();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much the picture shows over the plain background (any picture: .jpg, .png, .bmp, .tga, .gif, .dds, .mmp...)");
    }

    ImGui::SeparatorText("Map Editor");
    if (ImGui::Checkbox("Logic mode shows only the selected unit's logic", &lib.logicSelectedOnly)) lib.SaveConfig();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: every unit of the active map shows its paths, patrol points and radii");

    ImGui::Spacing();
    ImGui::SeparatorText("Found");
    ImGui::Text("%zu figures, %zu textures", lib.figureIndex.baseNames.size(), lib.textureIndex.names.size());
    if (!lib.maps.layers.empty()) {
        size_t terrains = 0, total = 0;
        for (const auto& m : lib.ListMapFiles()) { ++total; if (m.terrain) ++terrains; }
        ImGui::Text("%zu terrains (.mpr), %zu maps (.mob)", terrains, total - terrains);
    }
    if (lib.dbLoaded) {
        ImGui::Text("%s: %zu materials", lib.db.sourceLabel.c_str(), lib.db.materials.size());
        for (int c = 0; c < static_cast<int>(items::Category::Count); ++c) {
            ImGui::BulletText("%s: %zu", items::CategoryLabel(static_cast<items::Category>(c)),
                              lib.db.List(static_cast<items::Category>(c)).size());
        }
    }
    if (!st.message.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", st.message.c_str());
    }
}

// The Settings tab's right column: problem alerts, the Map Editor's mouse buttons and keys.
inline void ControlsPanel(Library& lib, SourcesState& st) {
    ImGui::SeparatorText("Problem alerts (database, map and script checks, File Processing jobs)");
    if (ImGui::Checkbox("Popup when errors are detected", &lib.alertPopups)) { alerts::SetPopups(lib.alertPopups); lib.SaveConfig(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("With a button to the tab that lists them.");
    if (ImGui::Checkbox("Sounds (SFX)", &lib.sfxEnabled)) { alerts::SetSound(lib.sfxEnabled); lib.SaveConfig(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A sound when errors or warnings are detected (one for each).");
    ImGui::SameLine();
    if (ImGui::SmallButton("Test error")) alerts::PlaySound(alerts::Level::Error);
    ImGui::SameLine();
    if (ImGui::SmallButton("Test warning")) alerts::PlaySound(alerts::Level::Warning);
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Volume##sfx", &lib.sfxVolume, 0, 100, "%d %%")) alerts::SetVolume(lib.sfxVolume);
    if (ImGui::IsItemDeactivatedAfterEdit()) lib.SaveConfig();
    ImGui::SeparatorText("Mouse (Map Editor and 3D Viewer)");
    // Mouse: the left button always selects (click) and draws selection rectangles (drag).
    {
        static const char* const buttons[] = {"Left", "Right button", "Middle button (wheel click)", "Side button 4", "Side button 5"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Orbit the camera with");
        ImGui::SameLine(220);
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##orbitbtn", buttons[lib.mapMouseOrbit])) {
            for (int b = 1; b <= 4; ++b) if (ImGui::Selectable(buttons[b], lib.mapMouseOrbit == b)) { lib.mapMouseOrbit = b; lib.SaveConfig(); }
            ImGui::EndCombo();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Drag the ground with");
        ImGui::SameLine(220);
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##panbtn", buttons[lib.mapMousePan])) {
            for (int b = 1; b <= 4; ++b) if (ImGui::Selectable(buttons[b], lib.mapMousePan == b)) { lib.mapMousePan = b; lib.SaveConfig(); }
            ImGui::EndCombo();
        }
        if (lib.mapMouseOrbit == lib.mapMousePan) Note("Orbit and drag use the same button: that button orbits.");
        Hint("The left button selects: click, Shift+click to add or remove, drag for a rectangle.");
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Map Editor keys");
    Hint("By key position: the defaults are the US layout's, shown with your layout's letters.");
    KeyBindings(lib, st);

}

// The whole Settings tab: sources and options on the left, mouse and keys on the right, each column
// scrolling on its own.
inline void SettingsTab(Library& lib, SourcesState& st) {
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::BeginChild("##settingsLeft", ImVec2(half, 0));
    SourcesTab(lib, st);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##settingsRight", ImVec2(0, 0));
    ControlsPanel(lib, st);
    ImGui::EndChild();
}

} // namespace ui
