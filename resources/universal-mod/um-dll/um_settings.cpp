// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: the unified settings table (um.cfg and the environment), um.cfg read and written.
// Shared declarations: um_internal.hpp.
#include "um_internal.hpp"

// ============================================================================
// Unified Settings Table
// ============================================================================
// Every um.cfg / environment-variable setting used to be hand-maintained in
// three separate places that had to be kept in sync by hand: the default
// um.cfg template writer, the um.cfg parser, and the environment-variable
// override pass in InitializeDllThread. All three are now driven by the one
// declarative table below (kSettings) - adding, renaming, or re-documenting a
// setting is a one-line change here instead of three separate edits.
//
// Note on OVERLAY_THREAD_COUNT: the original default-template writer printed
// the *current live value* of g_overlayThreadDisplayCount for this one
// setting (relevant only if um.cfg is deleted mid-session, after this setting
// was already changed by an earlier load, and then reloaded/regenerated).
// The table below prints the true fixed default (8) instead, which is more
// predictable for a file whose entire purpose is to *show the defaults*, so
// this is a deliberate small behavior simplification, not an oversight.



SettingDef BoolSetting(const char* key, bool* target, bool def, bool hasEnv,
        const char* comment, const char* section) {
    return SettingDef{ key, SettingType::Bool, target, nullptr, hasEnv,
        def, nullptr, 0, false, 0, 0, 0, 0, 0, nullptr, section, comment };
}

SettingDef StringSetting(const char* key, char* target, size_t bufferSize, const char* def,
        bool quoteInTemplate, bool hasEnv, const char* comment, const char* section) {
    return SettingDef{ key, SettingType::String, target, nullptr, hasEnv,
        false, def, bufferSize, quoteInTemplate, 0, 0, 0, 0, 0, nullptr, section, comment };
}

SettingDef VKeySetting(const char* key, BYTE* target, BYTE def, bool hasEnv,
        const char* comment, const char* section) {
    return SettingDef{ key, SettingType::VKey, target, nullptr, hasEnv,
        false, nullptr, 0, false, def, 0, 0, 0, 0, nullptr, section, comment };
}

SettingDef ColorSetting(const char* key, COLORREF* target, COLORREF def, bool hasEnv,
        const char* comment, const char* section) {
    return SettingDef{ key, SettingType::Color, target, nullptr, hasEnv,
        false, nullptr, 0, false, 0, def, 0, 0, 0, nullptr, section, comment };
}

SettingDef IntSetting(const char* key, int* target, long min, long max, long def, bool hasEnv,
        const char* comment, const char* section) {
    return SettingDef{ key, SettingType::ClampedInt, target, nullptr, hasEnv,
        false, nullptr, 0, false, 0, 0, min, max, def, nullptr, section, comment };
}

SettingDef PositionSetting(const char* key, char* target, size_t bufferSize, const char* def,
        bool hasEnv, const char* comment, const char* section) {
    return SettingDef{ key, SettingType::Position, target, nullptr, hasEnv,
        false, nullptr, bufferSize, false, 0, 0, 0, 0, 0, def, section, comment };
}

SettingDef FpsMarksSetting(const char* key, double* target, int* countTarget, const char* templateText,
        bool hasEnv, const char* comment, const char* section) {
    return SettingDef{ key, SettingType::FpsMarks, target, countTarget, hasEnv,
        false, templateText, 0, false, 0, 0, 0, 0, 0, nullptr, section, comment };
}

// Declared ahead of the table since a couple of entries' comment text is
// generated once from a compile-time constant rather than hand-copied.
const SettingDef kSettings[] = {
    BoolSetting("SPELLADDON_ASI_CHECK", &g_enableAsiCheck, true, true,
        "; Require SpellAddonX.asi beside game.exe; (true/false)"),
    BoolSetting("UM_ENGINE", &g_enableEngine, true, true,
        "; Load um-engine.dll and use its re-implemented game functions (each one switched in um-engine.cfg); (true/false)"),

    BoolSetting("KEYBOARD_REWRITES", &g_enableKeyboardRewrites, true, true,
        "; Rewrite backtick and number-row input as US-QWERTY keys; (true/false)",
        "; -- Keyboard --"),
    BoolSetting("KEYBOARD_REWRITES_LOGGING", &g_enableKeyboardRewriteLogging, false, true,
        "; Log keyboard rewrite events; (true/false)"),
    VKeySetting("RELOAD_CONFIG_KEY", &g_reloadConfigKey, VK_F12, true,
        "; Key that reloads this file without restarting the game (some settings still\n"
        "; need a restart); F1-F12 or a virtual-key code."),

    BoolSetting("LOGGING", &g_enableCrashLogging, true, true,
        "; Write diagnostic and crash information to um.log; (true/false)",
        "; -- Logging --"),
    BoolSetting("FILE_IO_LOGGING", &g_enableFileIoLogging, false, true,
        "; Log file opens, reads and writes; (true/false)"),
    StringSetting("FILE_IO_LOGGING_FILTER", g_fileIoLoggingFilter, sizeof(g_fileIoLoggingFilter), "", true, false,
        "; File extensions to leave out of file-I/O logging, comma-separated (e.g. mmp,res)."),
    BoolSetting("CLEAR_LOG_ON_START", &g_clearLogOnStart, true, true,
        "; Clear um.log when the game starts; (true/false)"),

    BoolSetting("SUPPRESS_ERROR_DIALOGS", &g_suppressErrorDialogs, true, true,
        "; Suppress Windows critical-error and crash dialogs, so the game just closes on\n"
        "; a crash (um.log and the dump are still written); (true/false)",
        "; -- Crash handling --"),
    BoolSetting("CRASH_DUMPS", &g_enableCrashDumps, true, false,
        "; Write a minidump (.dmp) next to um.log when the game crashes; (true/false)"),
    BoolSetting("MOB_VALIDATION", &g_enableMobValidation, false, true,
        "; Check this mod's .mob map files when opened and log problems (missing items or spells, script errors, shared object IDs); (true/false)"),
    IntSetting("HEAP_ALLOC_PADDING", &g_heapAllocPadding, 0, 256, 64, true,
        "; Spare bytes after each memory block, absorbing the game's heap overrun that crashes it on Wine. Restart needed; 0 = off (0-256)."),
    BoolSetting("HEAP_OVERRUN_REPORTS", &g_enableHeapFreeQuarantine, false, true,
        "; Diagnostics: tracks every allocation and reports in um.log which ones the game\n"
        "; overruns. Slows the game down and uses extra memory; needs a restart; (true/false)"),

    BoolSetting("OVERLAY_ENABLED", &g_enableOverlay, true, true,
        "; Show the diagnostic overlay on top of the game; (true/false)",
        "; -- Overlay --"),
    VKeySetting("OVERLAY_TOGGLE_KEY", &g_overlayToggleKey, VK_F9, true,
        "; Key that shows/hides the main panel; F1-F12 or a virtual-key code."),
    PositionSetting("OVERLAY_POSITION", g_overlayPosition, sizeof(g_overlayPosition), "top-left", true,
        "; Panel position: top-left, top-right, bottom-left, bottom-right, top, bottom,\n"
        "; left, right or center."),
    ColorSetting("OVERLAY_COLOR", &g_overlayTextColor, RGB(0, 255, 0), true,
        "; Text color as hex RRGGBB."),
    IntSetting("OVERLAY_REFRESH_MS", &g_overlayRefreshMs, 100, 5000, 500, true,
        "; Refresh interval in milliseconds (100-5000)."),
    BoolSetting("OVERLAY_COMPACT", &g_overlayCompact, false, true,
        "; Compact mode: only the FPS line (and the warning badge) instead of the full\n"
        "; panel; (true/false)"),
    BoolSetting("OVERLAY_SHOW_FPS_GRAPH", &g_overlayShowFpsGraph, true, true,
        "; Show the FPS graph; (true/false)"),
    FpsMarksSetting("OVERLAY_FPS_MARKS", g_overlayFpsMarks, &g_overlayFpsMarkCount, "30,60,75,120,140,165,240", true,
        "; FPS reference lines on the graph, comma-separated and ascending; the higher\n"
        "; ones appear once the game reaches them."),
    BoolSetting("OVERLAY_SHOW_FRAME_STATS", &g_overlayShowFrameStats, true, true,
        "; Show the 1% low FPS and worst frametime of the last 10 seconds; (true/false)"),
    BoolSetting("OVERLAY_SHOW_RESOURCES", &g_overlayShowResources, true, true,
        "; Show CPU and memory use of game.exe, and how much of its address space is used; (true/false)"),
    BoolSetting("OVERLAY_SHOW_BACKEND", &g_overlayShowBackend, true, true,
        "; Show the DirectDraw driver name and the full renderer chain (DxWrapper, dgVoodoo,\n"
        "; D7VK, DXVK, wined3d...); (true/false)"),
    BoolSetting("OVERLAY_SHOW_LAA", &g_overlayShowLaa, true, true,
        "; Show whether game.exe can use more than 2 GB of address space; (true/false)"),
    BoolSetting("OVERLAY_SHOW_MAP", &g_overlayShowMap, true, true,
        "; Show the current map (terrain, base map, quest map, and how many script maps its\n"
        "; script loaded) and the session time; (true/false)"),
    BoolSetting("OVERLAY_SHOW_WARNINGS", &g_overlayShowWarnings, true, true,
        "; Show a red badge with the number of warnings/errors logged so far; (true/false)"),
    BoolSetting("OVERLAY_SHOW_THREADS", &g_overlayShowThreads, true, true,
        "; Show CPU usage per thread; (true/false)"),
    IntSetting("OVERLAY_THREAD_COUNT", &g_overlayThreadDisplayCount, 1, OVERLAY_THREAD_DISPLAY_MAX, 8, true,
        "; Number of threads to list (1-32)."),
    ColorSetting("OVERLAY_BACKGROUND_COLOR", &g_overlayBackgroundColor, RGB(0, 0, 0), true,
        "; Background color as hex RRGGBB."),
    IntSetting("OVERLAY_BACKGROUND_OPACITY", &g_overlayBackgroundOpacityPercent, 0, 100, 20, true,
        "; Background opacity in percent (0-100). Anything below 100 can cost a lot of\n"
        "; FPS on some Wine/Wayland setups; 100 is the safe choice."),
    StringSetting("OVERLAY_TRANSPARENCY_STYLE", g_overlayTransparencyStyle, sizeof(g_overlayTransparencyStyle), "alpha", false, true,
        "; How opacity below 100 is drawn: alpha (smooth, not honored everywhere) or\n"
        "; dither (scanline pattern, works everywhere)."),

    BoolSetting("OVERLAY_LOG_ENABLED", &g_overlayLogEnabled, true, true,
        "; Show a panel with the latest um.log lines; (true/false)",
        "; -- Overlay log panel --"),
    VKeySetting("OVERLAY_LOG_TOGGLE_KEY", &g_overlayLogToggleKey, VK_F10, true,
        "; Key that shows/hides the log panel; F1-F12 or a virtual-key code."),
    IntSetting("OVERLAY_LOG_LINES", &g_overlayLogLineCount, 1, OVERLAY_LOG_CAPACITY, 10, true,
        "; Number of log lines to show, newest at the bottom (1-50)."),
    IntSetting("OVERLAY_LOG_WIDTH", &g_overlayLogPanelWidth, 300, 2000, 900, true,
        "; Log panel width in pixels (300-2000)."),
    BoolSetting("OVERLAY_LOG_WRAP", &g_overlayLogWrapEnabled, true, true,
        "; Wrap long lines onto extra rows instead of clipping them; (true/false)"),
    PositionSetting("OVERLAY_LOG_POSITION", g_overlayLogPosition, sizeof(g_overlayLogPosition), "bottom-left", true,
        "; Log panel position, same choices as OVERLAY_POSITION."),
    StringSetting("OVERLAY_LOG_LEVEL_FILTER", g_overlayLogLevelFilter, sizeof(g_overlayLogLevelFilter), "SYSINFO", true, true,
        "; Log levels to hide in the panel, comma-separated (SYSINFO, INFO, WARN, ERROR,\n"
        "; FATAL, DEBUG); um.log still keeps everything."),
    BoolSetting("OVERLAY_PROFILER_ENABLED", &g_profilerEnabled, false, true,
        "; Profiler window: where the main thread spends each frame. Restart needed; (true/false)",
        "; -- Profiler --"),
    VKeySetting("OVERLAY_PROFILER_KEY", &g_profilerKey, VK_F11, true,
        "; Key that shows/hides the profiler window (F1-F12 or a virtual-key code). Functions can be named in um-names.txt (\"address name\")."),
    PositionSetting("OVERLAY_PROFILER_POSITION", g_profilerPosition, sizeof(g_profilerPosition), "left", true,
        "; Profiler window position, same choices as OVERLAY_POSITION."),
    VKeySetting("OVERLAY_PROFILER_DUMP_KEY", &g_profilerDumpKey, VK_SCROLL, true,
        "; Key that writes the profiler's current tree (what its window shows) to um-profile.txt\n"
        "; beside um.log, with the time: send that file when reporting a slow scene. Scroll Lock by default."),
    IntSetting("OVERLAY_PROFILER_HZ", &g_profilerHz, 50, 1000, 250, true,
        "; How many times per second the main thread is sampled (50-1000); more is more\n"
        "; precise and costs more."),
    IntSetting("OVERLAY_PROFILER_LINES", &g_profilerLineCount, 6, 40, 20, true,
        "; Number of lines of the profiler tree (6-40)."),

    StringSetting("DDRAW_DLL", g_ddrawDll, sizeof(g_ddrawDll), "auto", true, false,
        "; DirectDraw wrapper replacing the Engine folder's ddraw.dll (the patched D7VK): auto = d7vk.dll under Wine, the game's own on Windows; or a path; empty = the game's.",
        "; -- Performance (see the profiler) --"),
    BoolSetting("MSAA", &g_enableMsaa, true, true,
        "; Sharp anti-aliasing (MSAA) through D7VK: 4x, or 2x if the GPU has no 4x (automatic). No blur; (true/false)"),
    BoolSetting("UI_QUAD_BLIT", &g_uiQuadBlit, true, true,
        "; Draw the interface and the software cursor as quads instead of blitting them onto the frame: saves ~3 ms per frame on D7VK, ~6 ms with the patched one (it needs this). (true/false)"),
    BoolSetting("BORDERLESS", &g_enableBorderless, false, true,
        "; Windowed game (fullscreen=0) acts like borderless fullscreen: stays active and unminimized when it loses the focus, mouse confined to it while focused. (true/false)",
        "; -- Window --"),
    BoolSetting("BORDERLESS_SCALE", &g_enableScale, false, true,
        "; With BORDERLESS: the window covers the whole monitor and the game's frame is scaled up to it (black bars keep the aspect ratio), so a lower resolution fills the screen. (true/false)"),
    BoolSetting("DLL_SERVER_ENABLED", &g_dllServerEnabled, false, true,
        "; Let um-multitool connect to the running game (this computer only, no password: it can read the game's memory). Restart needed; (true/false)",
        "; -- DLL server --"),
    IntSetting("DLL_SERVER_PORT", &g_dllServerPort, 1024, 65535, 18888, true,
        "; TCP port the DLL server listens on (1024-65535); the same as in um-multitool."),
    BoolSetting("DLL_SERVER_DEBUG", &g_dllServerDebug, false, true,
        "; DEVELOPMENT ONLY, DANGEROUS: lets the DLL server write the game's memory and run code inside game.exe. Never on a player's install. Restart needed; (true/false)"),
};
const size_t kSettingCount = sizeof(kSettings) / sizeof(kSettings[0]);

// Applies one already-extracted "KEY=value" pair to its backing global,
// dispatching on the setting's declared type. Used by both the um.cfg parser
// and (for everything except Bool - see ApplySettingEnvOverride) the
// environment-variable override pass, since both contexts want the same
// "authoritatively set this value" behavior.
static void ApplySettingValue(const SettingDef& def, const char* value) {
    switch (def.type) {
    case SettingType::Bool:
        *static_cast<bool*>(def.target) = IsTrueString(value);
        break;
    case SettingType::String:
        SetQuotedConfigString(static_cast<char*>(def.target), def.bufferSize, value);
        break;
    case SettingType::VKey:
        *static_cast<BYTE*>(def.target) = ParseVirtualKeyName(value, def.vkeyDefault);
        break;
    case SettingType::Color:
        *static_cast<COLORREF*>(def.target) = ParseHexColor(value, def.colorDefault);
        break;
    case SettingType::ClampedInt:
        *static_cast<int*>(def.target) = static_cast<int>(
            ParseClampedLong(value, def.intMin, def.intMax, def.intDefault));
        break;
    case SettingType::Position:
        ParseOverlayPosition(value, static_cast<char*>(def.target), def.bufferSize);
        break;
    case SettingType::FpsMarks:
        ParseFpsMarks(value, static_cast<double*>(def.target), static_cast<int*>(def.target2));
        break;
    }
}

// Applies an environment-variable override for one setting, if both the
// setting allows env overrides and that variable is actually set. Bool
// settings intentionally preserve their original asymmetric semantics here:
// an environment variable can only turn a flag ON (OR-merge with whatever
// um.cfg already set), never force one off, so a stray env var can't
// silently disable a safety feature the user deliberately enabled in their
// config file. Every other type fully overwrites, matching the original
// per-setting env-override code this table replaces.
void ApplySettingEnvOverride(const SettingDef& def) {
    if (!def.hasEnvOverride) {
        return;
    }
    const char* value = getenv(def.key);
    if (!value) {
        return;
    }
    if (def.type == SettingType::Bool) {
        *static_cast<bool*>(def.target) = *static_cast<bool*>(def.target) || IsTrueString(value);
    } else {
        ApplySettingValue(def, value);
    }
}

// Formats one setting's default value for the generated um.cfg template,
// matching exactly what ApplySettingValue(def, result) would parse back.
static void FormatSettingDefault(const SettingDef& def, char* out, size_t outSize) {
    switch (def.type) {
    case SettingType::Bool:
        snprintf(out, outSize, "%s", def.boolDefault ? "true" : "false");
        break;
    case SettingType::String:
    case SettingType::FpsMarks:
        snprintf(out, outSize, "%s", def.stringDefault ? def.stringDefault : "");
        break;
    case SettingType::VKey:
        // Every setting currently defaults to an F-key; hex is a defensive
        // fallback for any future non-F-key default, not currently reachable.
        if (def.vkeyDefault >= VK_F1 && def.vkeyDefault <= VK_F12) {
            snprintf(out, outSize, "F%d", def.vkeyDefault - VK_F1 + 1);
        } else {
            snprintf(out, outSize, "0x%02X", def.vkeyDefault);
        }
        break;
    case SettingType::Color:
        snprintf(out, outSize, "%02X%02X%02X",
            GetRValue(def.colorDefault), GetGValue(def.colorDefault), GetBValue(def.colorDefault));
        break;
    case SettingType::ClampedInt:
        snprintf(out, outSize, "%ld", def.intDefault);
        break;
    case SettingType::Position:
        snprintf(out, outSize, "%s", def.positionDefault ? def.positionDefault : "");
        break;
    }
}

// Writes a fresh um.cfg populated with every setting's default value and its
// documentation comment, in table order; section banners and the blank line
// separating each section are driven by the table too (a blank line is
// inserted after the last entry of each section, detected by the next
// entry - or end of table - starting a new section banner).
static void WriteDefaultConfigFile(FILE* file) {
    fprintf(file, "; Universal Mod Configuration\n\n");
    for (size_t i = 0; i < kSettingCount; ++i) {
        const SettingDef& def = kSettings[i];
        if (def.sectionBanner) {
            fprintf(file, "%s\n", def.sectionBanner);
        }
        if (def.comment) {
            fprintf(file, "%s\n", def.comment);
        }
        char valueText[512] = {};
        FormatSettingDefault(def, valueText, sizeof(valueText));
        if (def.type == SettingType::String && def.quoteInTemplate) {
            fprintf(file, "%s=\"%s\"\n", def.key, valueText);
        } else {
            fprintf(file, "%s=%s\n", def.key, valueText);
        }
        bool lastInSection = (i + 1 == kSettingCount) || (kSettings[i + 1].sectionBanner != nullptr);
        if (lastInSection) {
            fprintf(file, "\n");
        }
    }
}

// Load optional configuration overrides from um.cfg. Missing files are created
// with documented defaults; malformed or unknown lines are ignored.
void LoadConfigFile(const char* dllPath) {
    char configPath[MAX_PATH] = {};
    size_t dllPathLen = strlen(dllPath);
    if (dllPathLen >= sizeof(configPath)) {
        dllPathLen = sizeof(configPath) - 1;
    }
    memcpy(configPath, dllPath, dllPathLen);
    configPath[dllPathLen] = '\0';
    size_t lastSlash = strlen(configPath);
    while (lastSlash > 0 && configPath[lastSlash - 1] != '\\' && configPath[lastSlash - 1] != '/') {
        --lastSlash;
    }
    if (lastSlash > 0) {
        // Strip the separator itself, not just what follows it - the "\\um.log"/
        // "\\um.cfg" appended below already supply their own leading separator,
        // so keeping this one too produced a harmless but confusing doubled
        // backslash in every logged path (e.g. "Universal-Mod\\um-crashdump-...").
        configPath[lastSlash - 1] = '\0';
    }
    memcpy(g_logPath, configPath, strlen(configPath) + 1);
    strncat(g_logPath, "\\um.log", sizeof(g_logPath) - strlen(g_logPath) - 1);
    strncat(configPath, "\\um.cfg", sizeof(configPath) - strlen(configPath) - 1);

    FILE* file = fopen(configPath, "r");
    if (!file) {
        // Create um.cfg if it doesn't exist, populated with every setting's
        // default value and documentation from the table above.
        file = fopen(configPath, "w");
        if (file) {
            WriteDefaultConfigFile(file);
            fclose(file);
        }
        return;
    }

    char line[256] = {};
    while (fgets(line, sizeof(line), file)) {
        // trim leading whitespace
        char* start = line;
        while (*start && (*start == ' ' || *start == '\t')) {
            start++;
        }

        // skip empty lines and comments
        if (*start == '\0' || *start == ';' || *start == '#' || *start == '\n') {
            continue;
        }

        // find the '=' delimiter
        char* equals = strchr(start, '=');
        if (!equals) {
            continue;
        }

        // extract key
        char key[128] = {};
        size_t keyLen = equals - start;
        if (keyLen >= sizeof(key)) {
            continue;
        }
        strncpy(key, start, keyLen);
        key[keyLen] = '\0';

        // trim trailing whitespace from key
        while (keyLen > 0 && (key[keyLen - 1] == ' ' || key[keyLen - 1] == '\t')) {
            key[--keyLen] = '\0';
        }

        // extract value
        char* value = equals + 1;
        while (*value && (*value == ' ' || *value == '\t')) {
            value++;
        }

        // trim trailing whitespace and newline from value
        char* valueEnd = value + strlen(value) - 1;
        while (valueEnd > value && (*valueEnd == ' ' || *valueEnd == '\t' || *valueEnd == '\n' || *valueEnd == '\r')) {
            *valueEnd-- = '\0';
        }

        bool matched = false;
        for (size_t i = 0; i < kSettingCount; ++i) {
            if (EqualsIgnoreCase(key, kSettings[i].key)) {
                ApplySettingValue(kSettings[i], value);
                matched = true;
                break;
            }
        }
        if (!matched) {
            LogLine("WARN", "Unknown um.cfg setting '%s' ignored (typo?)", key);
        }
    }

    fclose(file);
}

// Re-reads um.cfg and re-applies whatever can safely take effect while the
// game is already running (most flags, overlay settings).
// A few things - installing IAT/DirectDraw hooks and creating the overlay
// window - only ever happen once at DLL attach, so toggling those settings
// on for the first time via reload still needs a game restart to take effect.
void ReloadConfiguration() {
    if (!g_dllModule) {
        return;
    }
    char dllPath[MAX_PATH] = {};
    if (GetModuleFileNameA(g_dllModule, dllPath, MAX_PATH) == 0) {
        LogLine("WARN", "Config reload failed: could not resolve um.dll's own path");
        return;
    }
    LoadConfigFile(dllPath);
    if (const char* autoDump = getenv("UM_PROFILE_AUTODUMP")) { // unattended profiling runs (#80)
        g_profilerAutoDumpSeconds = atoi(autoDump);
        if (g_profilerAutoDumpSeconds > 0) { g_profilerEnabled = true; g_profilerVisible = 1; LogLine("INFO", "UM_PROFILE_AUTODUMP=%d: the profiler runs from the start and dumps every %d s", g_profilerAutoDumpSeconds, g_profilerAutoDumpSeconds); }
    }
    LogLine("INFO", "um.cfg reloaded");
}

