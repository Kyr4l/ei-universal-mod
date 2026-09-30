#pragma once

// Entry points for each merged subtool (originally standalone main() functions).
int RunDdsMmp(int argc, char* argv[]);
int RunIniReg(int argc, char* argv[]);
int RunMobDump(int argc, char* argv[]);
int RunResTool(int argc, char* argv[]);
int RunXlsxDb(int argc, char* argv[]);

#include <cstdint>
#include <string>
#include <vector>

// inireg.cpp's conversions, in memory: INI text <-> the game's binary .reg (packed quests hold quest.reg).
bool IniRegTextToReg(const std::string& iniText, std::vector<uint8_t>& regOut);
bool IniRegRegToText(const std::vector<uint8_t>& reg, std::string& iniOut, std::string& err);
