// The program's log (#88): what failed and what was done, as a file beside the executable (um-multitool.log),
// a window in the GUI (Settings > Log) and, with `gui --verbose`, the console too. Every tab writes here when
// something fails (a file dialog, a file that cannot be read, a save), so Windows users can report what happened.
#pragma once

#include <string>
#include <vector>

namespace umlog {

enum class Level { Info, Warning, Error };

void Write(Level level, const std::string& text);
void SetVerbose(bool on); // also to stderr
bool Verbose();
std::string FilePath();   // the log file
std::vector<std::string> Lines(); // the last lines kept in memory (newest last)
void Clear();

// Inside the ImGui frame: the log window (when `open`).
void DrawWindow(bool* open);

} // namespace umlog
