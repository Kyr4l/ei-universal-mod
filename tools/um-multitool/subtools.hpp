#pragma once

// Entry points for each merged subtool (originally standalone main() functions).
int RunDdsMmp(int argc, char* argv[]);
int RunIniReg(int argc, char* argv[]);
int RunMobDump(int argc, char* argv[]);
int RunResTool(int argc, char* argv[]);
int RunXlsxDb(int argc, char* argv[]);
int RunDbExport(int argc, char* argv[]);

#include <cstdint>
#include <string>
#include <vector>

// inireg.cpp's conversions, in memory: INI text <-> the game's binary .reg (packed quests hold quest.reg).
bool IniRegTextToReg(const std::string& iniText, std::vector<uint8_t>& regOut);
bool IniRegRegToText(const std::vector<uint8_t>& reg, std::string& iniOut, std::string& err);

// ddsmmp.cpp: RGBA8 pixels (top-to-bottom) as a 32-bit DDS / a PNT3 .mmp (the Texture Editor's Save).
bool RgbaToDds(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba, std::vector<uint8_t>& ddsOut);
bool RgbaToMmp(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba, std::vector<uint8_t>& mmpOut, std::string& err);
