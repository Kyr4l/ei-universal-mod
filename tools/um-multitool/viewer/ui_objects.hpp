// Map objects (any figure: buildings, plants, props) for the 3D Viewer's Objects tab and the Map Editor's Add window:
// figure categories by name prefix, and which textures each figure wears in the maps (textures are not named after
// their figures: nafltr59 wears tree01..03, stst19 skeleton00), read once from every .mob of the map folders.
#pragma once

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "library.hpp"
#include "scene.hpp"
#include "ui_common.hpp"
#include "../mapedit/mob_file.hpp"

namespace objects {

inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline const char* const* Categories(int& count) {
    static const char* const kCats[] = {"All", "Buildings", "Walls & fences", "Bridges", "Structures", "Statues & ruins", "Ruins & props", "Plants",
                                        "Trees", "Stones", "Nature", "Items & props", "Containers", "Creature figures", "Effects & markers", "Other"};
    count = static_cast<int>(sizeof(kCats) / sizeof(kCats[0]));
    return kCats;
}

// The vanilla naming: stbuho1 = a building, naflli1 = a plant, initqi... = an item.
inline const char* Category(const std::string& lowerName) {
    static const std::pair<const char*, const char*> kPrefixes[] = {
        {"stbu", "Buildings"}, {"stwa", "Walls & fences"}, {"stbr", "Bridges"}, {"st", "Structures"}, {"jst", "Statues & ruins"},
        {"j", "Ruins & props"}, {"nafl", "Plants"}, {"natr", "Trees"}, {"nast", "Stones"}, {"na", "Nature"},
        {"in", "Items & props"}, {"co", "Containers"}, {"un", "Creature figures"}, {"ef", "Effects & markers"}};
    for (const auto& pr : kPrefixes) if (lowerName.rfind(pr.first, 0) == 0) return pr.second;
    return "Other";
}

// figure (lower case) -> the textures it wears in the maps, most used first (only those the texture sources have).
struct WornTextures {
    std::map<std::string, std::vector<std::string>> byFigure;
    bool built = false;
    void Build(const Library& lib, const std::vector<const mob::File*>& extra = {}) {
        if (built) return;
        built = true;
        std::map<std::string, std::map<std::string, int>> count;
        auto add = [&](const mob::File& f) {
            for (const mob::Object& o : f.objects)
                if (mob::HasFigure(o.kind) && !o.templ.empty() && !o.primTexture.empty()) ++count[Lower(o.templ)][o.primTexture];
        };
        for (const mob::File* f : extra) if (f) add(*f);
        for (const Library::MapFile& mf : lib.ListMapFiles()) {
            if (mf.terrain) continue;
            mob::File f;
            if (mob::Load(mf.path, f)) add(f);
        }
        for (auto& kv : count) {
            std::vector<std::pair<int, std::string>> v;
            for (auto& t : kv.second) v.push_back({-t.second, t.first});
            std::sort(v.begin(), v.end());
            for (auto& t : v) if (lib.textureIndex.Has(t.second)) byFigure[kv.first].push_back(t.second);
        }
    }
    // The most worn; else one named like the figure (or without its last digits); else the first starting so.
    std::string Suited(const Library& lib, const std::string& figure) const {
        const std::string low = Lower(figure);
        const auto worn = byFigure.find(low);
        if (worn != byFigure.end() && !worn->second.empty()) return worn->second.front();
        std::string base = low;
        while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();
        for (const std::string& want : {low, base})
            if (!want.empty() && lib.textureIndex.Has(want)) return want;
        for (const std::string& n : lib.textureIndex.names) if (!base.empty() && n.rfind(base, 0) == 0) return n;
        return "";
    }
};

inline WornTextures& Worn() { static WornTextures w; return w; }

// The texture drop-down: found automatically, the ones the figure wears, then every texture (filtered).
// `texture` empty = automatic. True when changed.
inline bool TextureCombo(const Library& lib, const std::string& figure, char* texture, size_t size, float width) {
    const WornTextures& w = Worn();
    const auto worn = w.byFigure.find(Lower(figure));
    const std::string suited = figure.empty() ? "" : w.Suited(lib, figure);
    const std::string shown = texture[0] ? std::string(texture) : suited.empty() ? "(none found)" : suited + " (found)";
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##texture", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("(found automatically)", texture[0] == '\0')) { texture[0] = '\0'; changed = true; }
        if (worn != w.byFigure.end() && !worn->second.empty()) {
            ImGui::TextDisabled("Worn by this figure in the maps:");
            for (const std::string& t : worn->second)
                if (ImGui::Selectable((t + "##worn").c_str(), t == texture)) { std::snprintf(texture, size, "%s", t.c_str()); changed = true; }
        }
        ImGui::Separator();
        static char filter[64] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##texfilter", "filter all the textures", filter, sizeof(filter));
        const std::string tf = Lower(filter);
        int listed = 0;
        for (const std::string& t : lib.textureIndex.names) {
            if (!tf.empty() && t.find(tf) == std::string::npos) continue;
            if (++listed > 1500) { ImGui::TextDisabled("(more: filter to narrow)"); break; }
            if (ImGui::Selectable((t + "##all").c_str(), t == texture)) { std::snprintf(texture, size, "%s", t.c_str()); changed = true; }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Found automatically, the textures this figure wears in the maps (most used first), then every texture.");
    return changed;
}

// ---- the 3D Viewer's Objects tab ---------------------------------------------------------------------------
struct TabState {
    char filter[64] = "";
    int category = 0;
    std::string figure, shown;
    char texture[128] = "";
    bool dirty = false;
};

inline void ObjectsTab(Library& lib, Scene& scene, TabState& st) {
    Worn().Build(lib);
    int n = 0;
    const char* const* cats = Categories(n);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##objfilter", "search the figures", st.filter, sizeof(st.filter));
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##objcat", &st.category, cats, n);
    ImGui::TextUnformatted("Texture");
    ImGui::SameLine();
    if (TextureCombo(lib, st.figure, st.texture, sizeof(st.texture), -1)) st.dirty = true;
    const std::string filter = Lower(st.filter);
    if (ImGui::BeginListBox("##objects", ImVec2(-1, -1))) {
        int shown = 0;
        for (const std::string& name : lib.figureIndex.baseNames) {
            const std::string low = Lower(name);
            if (!filter.empty() && low.find(filter) == std::string::npos) continue;
            if (st.category > 0 && std::string(Category(low)) != cats[st.category]) continue;
            if (++shown > 2000) { ImGui::TextDisabled("(more: search to narrow)"); break; }
            if (ImGui::Selectable(name.c_str(), name == st.figure) || ui::NavMovedHere()) { st.figure = name; st.texture[0] = '\0'; st.dirty = true; }
        }
        ImGui::EndListBox();
    }
    if (st.dirty || st.shown != st.figure) {
        st.dirty = false;
        const bool newFigure = st.shown != st.figure;
        st.shown = st.figure;
        if (st.figure.empty()) return;
        if (newFigure) scene.LoadModel(lib, st.figure, true);
        scene.textureName = st.texture[0] ? std::string(st.texture) : Worn().Suited(lib, st.figure);
        scene.options.atlasUvs = Lower(st.figure).rfind("in", 0) == 0; // item figures use the 256 atlas, scenery its texture directly
    }
}

} // namespace objects
