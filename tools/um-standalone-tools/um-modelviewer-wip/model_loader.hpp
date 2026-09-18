// Loads a named figure (simple .fig/.bon pair or composite .mod/.bon/.anm set)
// from an AssetSource (loose directory or RES archive).
#pragma once

#include <map>
#include <string>

#include "asset_source.hpp"
#include "figure_format.hpp"

struct LoadedModel {
    fig::Model model;
    std::map<std::string, fig::AnimClip> animClips; // empty if no .anm
    std::string error;
    bool ok = false;
};

inline bool LoadNamedModel(const LayeredAssetSource& figSource, const std::string& baseName, LoadedModel& out) {
    out = LoadedModel{};

    // Resolve .lnk redirects first (attachment-socket aliases like initwecb4 -> initwecb4weapon).
    std::string resolvedName = baseName;
    std::vector<uint8_t> lnkData;
    if (figSource.ReadFile(baseName + ".lnk", lnkData)) {
        std::string suffix;
        if (fig::ParseLnk(lnkData.data(), lnkData.size(), suffix)) {
            std::string candidate = baseName + suffix;
            if (figSource.Contains(candidate + ".fig") || figSource.Contains(candidate + ".mod")) {
                resolvedName = candidate;
            } else if (figSource.Contains(suffix + ".fig") || figSource.Contains(suffix + ".mod")) {
                // Known vanilla anomaly (see figure-format.md): a .lnk that stores the
                // absolute target name instead of a suffix to append.
                resolvedName = suffix;
            }
        }
    }

    std::vector<uint8_t> modData;
    if (figSource.ReadFile(resolvedName + ".mod", modData)) {
        res::Archive modArchive;
        std::string err;
        if (!res::ParseArchive(modData, modArchive, err)) {
            out.error = "failed to parse " + resolvedName + ".mod: " + err;
            return false;
        }
        res::Archive bonArchive; // may stay empty if no sibling .bon exists
        std::vector<uint8_t> bonData;
        if (figSource.ReadFile(resolvedName + ".bon", bonData)) {
            std::string bonErr;
            res::ParseArchive(bonData, bonArchive, bonErr); // best-effort; composite .bon should always parse
        }
        if (!fig::LoadCompositeModel(modArchive, bonArchive, resolvedName, out.model)) {
            out.error = "failed to build hierarchy for " + resolvedName;
            return false;
        }

        std::vector<uint8_t> anmData;
        if (figSource.ReadFile(resolvedName + ".anm", anmData)) {
            std::string anmErr;
            fig::ParseAnm(anmData, out.animClips, anmErr); // best-effort
        }
        out.ok = true;
        return true;
    }

    std::vector<uint8_t> figData;
    if (figSource.ReadFile(resolvedName + ".fig", figData)) {
        std::vector<uint8_t> bonData;
        const std::vector<uint8_t>* bonPtr = nullptr;
        if (figSource.ReadFile(resolvedName + ".bon", bonData)) bonPtr = &bonData;
        if (!fig::LoadSimpleFigure(figData, bonPtr, resolvedName, out.model)) {
            out.error = "failed to parse " + resolvedName + ".fig";
            return false;
        }
        out.ok = true;
        return true;
    }

    out.error = "no .mod or .fig found for '" + baseName + "' (resolved: '" + resolvedName + "')";
    return false;
}
