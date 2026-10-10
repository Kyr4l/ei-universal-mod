// The File Processing tab's "DB" sub-tab: opens a gameplay database (.res, .xlsx or .ods), shows its
// sheets as editable tables with the problems the checks find (db_model.hpp) highlighted, and saves it
// as a spreadsheet or compiles it to a .res. Everything lives in db_editor.cpp.
#pragma once

#include <functional>
#include <string>

namespace dbedit {

struct Hooks {
    // The native file dialog (gui_main.cpp): save = a "save as" dialog.
    std::function<bool(bool save, const char* filterName, const char* filterExt, std::string& path)> pickFile;
    std::function<void()> show; // switches to File Processing > DB (the alerts' button)
    // The Settings' database (Sources) and the "open it automatically" option, saved in the config.
    std::function<std::string()> settingsDatabase;
    std::function<bool()> autoLoad;
    std::function<void(bool)> setAutoLoad;
    // The last "Compile to" of a database (by its path; "" = none yet), remembered in the config.
    std::function<std::string(const std::string& database)> compileTo;
    std::function<void(const std::string& database, const std::string& res)> setCompileTo;
    // Settings > Checks > Database checks (missing: on). Off, the database is not checked.
    std::function<bool()> checksOn;
};

void SetHooks(Hooks hooks);
void Update();           // every frame: opens the Settings' database when asked to
void DrawTab();          // inside the DB sub-tab
void OpenFile(const std::string& path);
bool HasUnsavedChanges();

} // namespace dbedit
