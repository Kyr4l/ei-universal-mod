/**
 * ============================================================================
 * um-inireg - High-Performance Evil Islands INI <-> REG Converter
 * ============================================================================
 *
 * Description:
 *   Optimized CLI tool to convert Evil Islands configuration files between:
 *     1. Standard plain-text INI files (.ini)
 *     2. Game-specific binary registry database files (.reg) with magic 0x45AB3EFB
 *
 * Features:
 *   - Fast single-pass binary serialization/deserialization in native C++17.
 *   - Exact 100% byte-for-byte binary compatibility with original Evil Islands tools.
 *   - Support for single-value and multi-value/array keys (integers, floats, strings).
 *   - Multithreaded batch processing for directories (-m / --multi).
 *   - Safe read-only file access (never modifies input files).
 *   - Support for custom output directory or file path (-o / --output).
 *   - Dry-run simulation mode (--dry-run).
 *   - Linux native and Windows cross-compilable (.exe).
 *
 * Usage:
 *   um-inireg [options] <path/to/file.ini|file.reg>
 *   um-inireg [options] -d <path/to/directory>
 *
 * Version:
 *   0.1
 * ============================================================================
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>

namespace fs = std::filesystem;

// Program metadata
static constexpr const char* PROGRAM_VERSION = "0.1";
static constexpr const char* PROGRAM_NAME = "um-inireg";
static constexpr uint32_t REG_MAGIC = 0x45AB3EFBu; // 'fb 3e ab 45'

// Value type tags
enum RegTag : uint8_t {
    TAG_INT32        = 0x00,
    TAG_FLOAT        = 0x01,
    TAG_STRING       = 0x02,
    TAG_ARRAY_INT32  = 0x80,
    TAG_ARRAY_FLOAT  = 0x81,
    TAG_ARRAY_STRING = 0x82
};

// ============================================================================
// Hash Table & Checksum Utilities
// ============================================================================

/**
 * Calculates the Evil Islands hash for section/key names.
 * Sum of ASCII lowercase character codes modulo bucket count.
 */
static inline uint16_t CalculateEiHash(const std::string& name, uint16_t bucketCount) {
    if (bucketCount == 0) return 0;
    uint32_t sum = 0;
    for (unsigned char c : name) {
        sum += static_cast<unsigned char>(std::tolower(c));
    }
    return static_cast<uint16_t>(sum % bucketCount);
}

/**
 * Hash table entry (6 bytes in binary representation):
 *   uint16_t next_index  (0xFFFF = end of chain)
 *   uint32_t offset      (file offset or section-relative offset)
 */
struct HashEntry {
    uint16_t nextIndex = 0xFFFF;
    uint32_t offset = 0;
};

/**
 * Constructs the Evil Islands hash table using backwards-scanning collision resolution.
 */
static std::vector<HashEntry> BuildHashTable(
    const std::vector<std::string>& names,
    const std::vector<uint32_t>& offsets)
{
    uint16_t n = static_cast<uint16_t>(names.size());
    std::vector<HashEntry> table(n);

    for (uint16_t i = 0; i < n; ++i) {
        const std::string& name = names[i];
        uint32_t off = offsets[i];
        uint16_t bucket = CalculateEiHash(name, n);

        if (table[bucket].offset == 0) {
            // First item in this bucket
            table[bucket].nextIndex = 0xFFFF;
            table[bucket].offset = off;
        } else {
            // Collision: follow chain to the end
            uint16_t curr = bucket;
            while (table[curr].nextIndex != 0xFFFF) {
                curr = table[curr].nextIndex;
            }

            // Find first available slot from the end
            int freeIdx = n - 1;
            while (freeIdx >= 0 && table[freeIdx].offset != 0) {
                --freeIdx;
            }

            if (freeIdx >= 0) {
                table[curr].nextIndex = static_cast<uint16_t>(freeIdx);
                table[freeIdx].nextIndex = 0xFFFF;
                table[freeIdx].offset = off;
            }
        }
    }

    return table;
}

// ============================================================================
// Data Representation Structures
// ============================================================================

enum class ValueType {
    Int32,
    Float,
    String
};

struct KeyEntry {
    std::string name;
    std::vector<std::string> rawValues;
};

struct SectionEntry {
    std::string name;
    std::vector<KeyEntry> keys;
};

struct IniDocument {
    std::vector<SectionEntry> sections;
};

// ============================================================================
// Value Classification & Formatting
// ============================================================================

/**
 * Checks if a string is a valid integer (optional leading +/- followed by digits).
 */
static bool IsInteger(const std::string& s, int32_t& outVal) {
    if (s.empty()) return false;
    size_t start = 0;
    if (s[0] == '-' || s[0] == '+') {
        if (s.size() == 1) return false;
        start = 1;
    }
    for (size_t i = start; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    try {
        outVal = static_cast<int32_t>(std::stol(s));
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * Checks if a string is a valid float (must contain '.' or 'e'/'E').
 */
static bool IsFloat(const std::string& s, float& outVal) {
    if (s.empty()) return false;
    bool hasDotOrExp = false;
    for (char c : s) {
        if (c == '.' || c == 'e' || c == 'E') {
            hasDotOrExp = true;
            break;
        }
    }
    if (!hasDotOrExp) return false;

    try {
        size_t idx = 0;
        outVal = std::stof(s, &idx);
        return idx == s.size();
    } catch (...) {
        return false;
    }
}

/**
 * Classifies a raw string value into Int32, Float, or String.
 */
static ValueType ClassifyValue(const std::string& s, int32_t& outInt, float& outFloat) {
    if (IsInteger(s, outInt)) {
        return ValueType::Int32;
    }
    if (IsFloat(s, outFloat)) {
        return ValueType::Float;
    }
    return ValueType::String;
}

/**
 * Formats a 32-bit float matching Evil Islands compact representation.
 */
static std::string FormatFloat(float val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6g", val);
    return std::string(buf);
}

// ============================================================================
// Little-Endian Binary Buffer Helpers
// ============================================================================

static inline void WriteUint8(std::vector<uint8_t>& buf, uint8_t val) {
    buf.push_back(val);
}

static inline void WriteUint16LE(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

static inline void WriteUint32LE(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

static inline void WriteInt32LE(std::vector<uint8_t>& buf, int32_t val) {
    WriteUint32LE(buf, static_cast<uint32_t>(val));
}

static inline void WriteFloatLE(std::vector<uint8_t>& buf, float val) {
    uint32_t u;
    std::memcpy(&u, &val, sizeof(u));
    WriteUint32LE(buf, u);
}

static inline void WriteBytes(std::vector<uint8_t>& buf, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + size);
}

static inline uint8_t ReadUint8(const uint8_t* p) {
    return *p;
}

static inline uint16_t ReadUint16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static inline uint32_t ReadUint32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
}

static inline int32_t ReadInt32LE(const uint8_t* p) {
    return static_cast<int32_t>(ReadUint32LE(p));
}

static inline float ReadFloatLE(const uint8_t* p) {
    uint32_t u = ReadUint32LE(p);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// ============================================================================
// INI -> REG Serialization
// ============================================================================

/**
 * Trims leading and trailing whitespace from a string.
 */
static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/**
 * Parses an INI text file into structured sections and key-value pairs.
 */
static bool ParseIniText(const std::string& text, IniDocument& doc) {
    std::istringstream stream(text);
    std::string line;
    SectionEntry* currentSec = nullptr;

    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue; // Skip comments and blank lines
        }

        if (line.front() == '[' && line.back() == ']') {
            std::string secName = Trim(line.substr(1, line.size() - 2));
            doc.sections.push_back({secName, {}});
            currentSec = &doc.sections.back();
        } else if (size_t eqPos = line.find('='); eqPos != std::string::npos) {
            if (!currentSec) continue;
            std::string key = Trim(line.substr(0, eqPos));
            std::string val = Trim(line.substr(eqPos + 1));

            // Check if key already exists in this section (multi-value)
            auto it = std::find_if(currentSec->keys.begin(), currentSec->keys.end(),
                                   [&](const KeyEntry& k) { return k.name == key; });
            if (it != currentSec->keys.end()) {
                it->rawValues.push_back(val);
            } else {
                currentSec->keys.push_back({key, {val}});
            }
        }
    }

    return true;
}

/**
 * Encodes a key-value record into binary format.
 */
static std::vector<uint8_t> EncodeKeyRecord(const KeyEntry& key) {
    std::vector<uint8_t> blob;
    uint16_t keyLen = static_cast<uint16_t>(key.name.size());

    // Single value encoding
    if (key.rawValues.size() == 1) {
        const std::string& v = key.rawValues[0];
        int32_t iVal = 0;
        float fVal = 0.0f;
        ValueType vt = ClassifyValue(v, iVal, fVal);

        if (vt == ValueType::Int32) {
            WriteUint8(blob, TAG_INT32);
            WriteUint16LE(blob, keyLen);
            WriteBytes(blob, key.name.data(), keyLen);
            WriteInt32LE(blob, iVal);
        } else if (vt == ValueType::Float) {
            WriteUint8(blob, TAG_FLOAT);
            WriteUint16LE(blob, keyLen);
            WriteBytes(blob, key.name.data(), keyLen);
            WriteFloatLE(blob, fVal);
        } else {
            uint16_t sLen = static_cast<uint16_t>(v.size());
            WriteUint8(blob, TAG_STRING);
            WriteUint16LE(blob, keyLen);
            WriteBytes(blob, key.name.data(), keyLen);
            WriteUint16LE(blob, sLen);
            WriteBytes(blob, v.data(), sLen);
        }
        return blob;
    }

    // Multi-value / array encoding
    uint16_t count = static_cast<uint16_t>(key.rawValues.size());
    bool allInt = true;
    bool allFloat = true;
    std::vector<int32_t> intVals;
    std::vector<float> floatVals;

    for (const auto& v : key.rawValues) {
        int32_t iVal = 0;
        float fVal = 0.0f;
        ValueType vt = ClassifyValue(v, iVal, fVal);

        if (vt == ValueType::Int32) {
            intVals.push_back(iVal);
            allFloat = false;
        } else if (vt == ValueType::Float) {
            floatVals.push_back(fVal);
            allInt = false;
        } else {
            allInt = false;
            allFloat = false;
        }
    }

    if (allInt) {
        WriteUint8(blob, TAG_ARRAY_INT32);
        WriteUint16LE(blob, keyLen);
        WriteBytes(blob, key.name.data(), keyLen);
        WriteUint16LE(blob, count);
        for (int32_t iv : intVals) {
            WriteInt32LE(blob, iv);
        }
    } else if (allFloat) {
        WriteUint8(blob, TAG_ARRAY_FLOAT);
        WriteUint16LE(blob, keyLen);
        WriteBytes(blob, key.name.data(), keyLen);
        WriteUint16LE(blob, count);
        for (float fv : floatVals) {
            WriteFloatLE(blob, fv);
        }
    } else {
        // Heterogeneous or string array
        WriteUint8(blob, TAG_ARRAY_STRING);
        WriteUint16LE(blob, keyLen);
        WriteBytes(blob, key.name.data(), keyLen);
        WriteUint16LE(blob, count);
        for (const auto& v : key.rawValues) {
            uint16_t sLen = static_cast<uint16_t>(v.size());
            WriteUint16LE(blob, sLen);
            WriteBytes(blob, v.data(), sLen);
        }
    }

    return blob;
}

/**
 * Converts an INI document to Evil Islands .reg binary format.
 */
static std::vector<uint8_t> ConvertIniToReg(const IniDocument& doc) {
    struct SectionPayload {
        std::string name;
        std::vector<uint8_t> buffer;
    };

    std::vector<SectionPayload> secPayloads;

    for (const auto& sec : doc.sections) {
        uint16_t numKeys = static_cast<uint16_t>(sec.keys.size());
        uint16_t nameLen = static_cast<uint16_t>(sec.name.size());

        // Header size: 2 (numKeys) + 2 (nameLen) + nameLen + numKeys * 6 (hash table)
        uint32_t headerSize = 4 + nameLen + numKeys * 6;

        std::vector<std::string> keyNames;
        std::vector<uint32_t> keyOffsets;
        std::vector<std::vector<uint8_t>> keyBlobs;

        uint32_t currentKeyOff = headerSize;
        for (const auto& k : sec.keys) {
            std::vector<uint8_t> blob = EncodeKeyRecord(k);
            keyNames.push_back(k.name);
            keyOffsets.push_back(currentKeyOff);
            currentKeyOff += static_cast<uint32_t>(blob.size());
            keyBlobs.push_back(std::move(blob));
        }

        std::vector<HashEntry> keyHashTable = BuildHashTable(keyNames, keyOffsets);

        std::vector<uint8_t> secBuf;
        WriteUint16LE(secBuf, numKeys);
        WriteUint16LE(secBuf, nameLen);
        WriteBytes(secBuf, sec.name.data(), nameLen);

        for (const auto& entry : keyHashTable) {
            WriteUint16LE(secBuf, entry.nextIndex);
            WriteUint32LE(secBuf, entry.offset);
        }

        for (const auto& blob : keyBlobs) {
            WriteBytes(secBuf, blob.data(), blob.size());
        }

        secPayloads.push_back({sec.name, std::move(secBuf)});
    }

    uint16_t numSections = static_cast<uint16_t>(secPayloads.size());
    uint32_t fileHeaderSize = 6 + numSections * 6;

    std::vector<std::string> secNames;
    std::vector<uint32_t> secOffsets;
    uint32_t currentSecOff = fileHeaderSize;

    for (const auto& sp : secPayloads) {
        secNames.push_back(sp.name);
        secOffsets.push_back(currentSecOff);
        currentSecOff += static_cast<uint32_t>(sp.buffer.size());
    }

    std::vector<HashEntry> secHashTable = BuildHashTable(secNames, secOffsets);

    std::vector<uint8_t> finalReg;
    finalReg.reserve(currentSecOff);

    WriteUint32LE(finalReg, REG_MAGIC);
    WriteUint16LE(finalReg, numSections);

    for (const auto& entry : secHashTable) {
        WriteUint16LE(finalReg, entry.nextIndex);
        WriteUint32LE(finalReg, entry.offset);
    }

    for (const auto& sp : secPayloads) {
        WriteBytes(finalReg, sp.buffer.data(), sp.buffer.size());
    }

    return finalReg;
}

// ============================================================================
// REG -> INI Deserialization
// ============================================================================

/**
 * Parses an Evil Islands .reg binary buffer into plain-text INI format.
 */
static bool ConvertRegToIni(const uint8_t* data, size_t size, std::string& iniOut, std::string& err) {
    if (size < 6) {
        err = "File is too small to contain a valid .reg header";
        return false;
    }

    uint32_t magic = ReadUint32LE(data);
    if (magic != REG_MAGIC) {
        err = "Invalid .reg magic header (expected 0x45AB3EFB)";
        return false;
    }

    uint16_t numSections = ReadUint16LE(data + 4);
    if (6 + static_cast<size_t>(numSections) * 6 > size) {
        err = "Corrupted section table (extends beyond file size)";
        return false;
    }

    // Collect all unique section offsets
    std::vector<uint32_t> secOffsets;
    for (uint16_t s = 0; s < numSections; ++s) {
        uint32_t off = ReadUint32LE(data + 6 + s * 6 + 2);
        if (off > 0 && off < size) {
            if (std::find(secOffsets.begin(), secOffsets.end(), off) == secOffsets.end()) {
                secOffsets.push_back(off);
            }
        }
    }
    std::sort(secOffsets.begin(), secOffsets.end());

    std::ostringstream out;

    for (uint32_t soff : secOffsets) {
        if (soff + 4 > size) continue;
        uint16_t numKeys = ReadUint16LE(data + soff);
        uint16_t secNameLen = ReadUint16LE(data + soff + 2);

        if (soff + 4 + static_cast<size_t>(secNameLen) + static_cast<size_t>(numKeys) * 6 > size) continue;
        std::string secName(reinterpret_cast<const char*>(data + soff + 4), secNameLen);
        out << "[" << secName << "]\n";

        uint32_t htOffset = soff + 4 + secNameLen;
        std::vector<uint32_t> keyOffsets;
        for (uint16_t k = 0; k < numKeys; ++k) {
            uint32_t koff = ReadUint32LE(data + htOffset + k * 6 + 2);
            if (koff > 0 && soff + koff < size) {
                if (std::find(keyOffsets.begin(), keyOffsets.end(), koff) == keyOffsets.end()) {
                    keyOffsets.push_back(koff);
                }
            }
        }
        std::sort(keyOffsets.begin(), keyOffsets.end());

        for (uint32_t koff : keyOffsets) {
            size_t kp = soff + koff;
            if (kp + 3 > size) continue;

            uint8_t tag = ReadUint8(data + kp);
            uint16_t kNameLen = ReadUint16LE(data + kp + 1);
            if (kp + 3 + kNameLen > size) continue;

            std::string keyName(reinterpret_cast<const char*>(data + kp + 3), kNameLen);
            size_t valPos = kp + 3 + kNameLen;

            bool isArray = (tag & 0x80) != 0;
            uint8_t baseType = tag & 0x7F;

            if (!isArray) {
                if (baseType == TAG_INT32) {
                    if (valPos + 4 <= size) {
                        int32_t iv = ReadInt32LE(data + valPos);
                        out << keyName << "=" << iv << "\n";
                    }
                } else if (baseType == TAG_FLOAT) {
                    if (valPos + 4 <= size) {
                        float fv = ReadFloatLE(data + valPos);
                        out << keyName << "=" << FormatFloat(fv) << "\n";
                    }
                } else if (baseType == TAG_STRING) {
                    if (valPos + 2 <= size) {
                        uint16_t sLen = ReadUint16LE(data + valPos);
                        if (valPos + 2 + sLen <= size) {
                            std::string strVal(reinterpret_cast<const char*>(data + valPos + 2), sLen);
                            out << keyName << "=" << strVal << "\n";
                        }
                    }
                }
            } else {
                if (valPos + 2 <= size) {
                    uint16_t count = ReadUint16LE(data + valPos);
                    valPos += 2;

                    for (uint16_t i = 0; i < count; ++i) {
                        if (baseType == TAG_INT32) {
                            if (valPos + 4 <= size) {
                                int32_t iv = ReadInt32LE(data + valPos);
                                valPos += 4;
                                out << keyName << "=" << iv << "\n";
                            }
                        } else if (baseType == TAG_FLOAT) {
                            if (valPos + 4 <= size) {
                                float fv = ReadFloatLE(data + valPos);
                                valPos += 4;
                                out << keyName << "=" << FormatFloat(fv) << "\n";
                            }
                        } else if (baseType == TAG_STRING) {
                            if (valPos + 2 <= size) {
                                uint16_t sLen = ReadUint16LE(data + valPos);
                                valPos += 2;
                                if (valPos + sLen <= size) {
                                    std::string strVal(reinterpret_cast<const char*>(data + valPos), sLen);
                                    valPos += sLen;
                                    out << keyName << "=" << strVal << "\n";
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    iniOut = out.str();
    return true;
}

// ============================================================================
// File Processing & CLI Orchestration
// ============================================================================

enum class ConvertMode {
    Auto,
    IniToReg,
    RegToIni
};

struct ConvertResult {
    bool success = false;
    fs::path inputPath;
    fs::path outputPath;
    size_t inSize = 0;
    size_t outSize = 0;
    std::string errorMessage;
};

static ConvertResult ProcessFile(
    const fs::path& inputPath,
    const fs::path& outputDirOrFile,
    ConvertMode mode,
    bool dryRun)
{
    ConvertResult res;
    res.inputPath = inputPath;

    // Read input file into memory strictly read-only
    std::ifstream in(inputPath, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        res.errorMessage = "Failed to open input file for reading";
        return res;
    }
    std::streamsize size = in.tellg();
    res.inSize = static_cast<size_t>(size);
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> inBuffer(res.inSize);
    if (res.inSize > 0) {
        if (!in.read(reinterpret_cast<char*>(inBuffer.data()), size)) {
            res.errorMessage = "Failed to read input file bytes";
            return res;
        }
    }
    in.close();

    // Determine conversion direction
    ConvertMode actualMode = mode;
    if (actualMode == ConvertMode::Auto) {
        std::string ext = inputPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".ini") {
            actualMode = ConvertMode::IniToReg;
        } else if (ext == ".reg") {
            actualMode = ConvertMode::RegToIni;
        } else {
            // Check binary magic
            if (inBuffer.size() >= 4 && ReadUint32LE(inBuffer.data()) == REG_MAGIC) {
                actualMode = ConvertMode::RegToIni;
            } else {
                actualMode = ConvertMode::IniToReg;
            }
        }
    }

    std::string targetExt = (actualMode == ConvertMode::IniToReg) ? ".reg" : ".ini";

    // Determine output file path
    if (outputDirOrFile.empty()) {
        res.outputPath = inputPath.parent_path() / (inputPath.stem().string() + targetExt);
    } else {
        std::error_code ec;
        bool isDir = fs::is_directory(outputDirOrFile, ec) || outputDirOrFile.extension().empty();
        if (isDir) {
            res.outputPath = outputDirOrFile / (inputPath.stem().string() + targetExt);
        } else {
            res.outputPath = outputDirOrFile;
        }
    }

    std::vector<uint8_t> outBuffer;

    if (actualMode == ConvertMode::IniToReg) {
        std::string iniText(reinterpret_cast<const char*>(inBuffer.data()), inBuffer.size());
        IniDocument doc;
        if (!ParseIniText(iniText, doc)) {
            res.errorMessage = "Failed to parse INI text syntax";
            return res;
        }
        outBuffer = ConvertIniToReg(doc);
    } else {
        std::string iniOut;
        std::string err;
        if (!ConvertRegToIni(inBuffer.data(), inBuffer.size(), iniOut, err)) {
            res.errorMessage = err;
            return res;
        }
        outBuffer.assign(iniOut.begin(), iniOut.end());
    }

    res.outSize = outBuffer.size();

    if (dryRun) {
        res.success = true;
        return res;
    }

    // Ensure parent directory exists
    if (res.outputPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(res.outputPath.parent_path(), ec);
    }

    // Write output file
    std::ofstream out(res.outputPath, std::ios::binary);
    if (!out.is_open()) {
        res.errorMessage = "Failed to open output file for writing: " + res.outputPath.string();
        return res;
    }
    if (!outBuffer.empty()) {
        out.write(reinterpret_cast<const char*>(outBuffer.data()), outBuffer.size());
    }
    if (!out) {
        res.errorMessage = "Failed to complete writing output file: " + res.outputPath.string();
        return res;
    }

    res.success = true;
    return res;
}

// ============================================================================
// CLI Argument Parsing & Batch Engine
// ============================================================================

struct CliOptions {
    bool showHelp = false;
    bool showVersion = false;
    bool multiThread = false;
    bool isDirMode = false;
    bool dryRun = false;
    ConvertMode mode = ConvertMode::Auto;
    fs::path inputPath;
    fs::path outputTarget;
};

static void PrintVersion() {
    std::cout << PROGRAM_NAME << " version " << PROGRAM_VERSION << "\n";
}

static void PrintHelp() {
    std::cout << "um-inireg - High-Performance Evil Islands INI <-> REG Converter\n\n"
              << "Usage:\n"
              << "  um-inireg [options] <path/to/file.ini|file.reg>\n"
              << "  um-inireg [options] -d <path/to/directory>\n\n"
              << "Options:\n"
              << "  -o, --output <path>   Set output directory or file path\n"
              << "                        (default: same directory with swapped extension)\n"
              << "  -d, --dir <dir>       Process all .ini or .reg files in the directory\n"
              << "  -m, --multi           Process multiple files in parallel across CPU cores\n"
              << "                        (only active when used together with -d / --dir)\n"
              << "  --ini2reg             Force INI -> REG conversion mode\n"
              << "  --reg2ini             Force REG -> INI conversion mode\n"
              << "  --dry-run             Show what the program would do without writing files\n"
              << "  -v, --version         Print program version (" << PROGRAM_VERSION << ")\n"
              << "  -h, --help            Print this help message\n\n"
              << "Examples:\n"
              << "  um-inireg config.ini                 # Creates config.reg\n"
              << "  um-inireg config.reg                 # Creates config.ini\n"
              << "  um-inireg -d ./ini -o ./reg -m       # Batch converts all INIs to REGs in parallel\n"
              << "  um-inireg -d ./reg --reg2ini         # Batch converts all REGs back to INIs\n";
}

static bool ParseCommandLine(int argc, char* argv[], CliOptions& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opt.showHelp = true;
            return true;
        } else if (arg == "-v" || arg == "--version") {
            opt.showVersion = true;
            return true;
        } else if (arg == "--dry-run") {
            opt.dryRun = true;
        } else if (arg == "-m" || arg == "--multi") {
            opt.multiThread = true;
        } else if (arg == "--ini2reg") {
            opt.mode = ConvertMode::IniToReg;
        } else if (arg == "--reg2ini") {
            opt.mode = ConvertMode::RegToIni;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                opt.outputTarget = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires a path argument.\n";
                return false;
            }
        } else if (arg.rfind("--output=", 0) == 0) {
            opt.outputTarget = arg.substr(9);
        } else if (arg == "-d" || arg == "--dir") {
            opt.isDirMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                opt.inputPath = argv[++i];
            }
        } else if (arg.rfind("--dir=", 0) == 0) {
            opt.isDirMode = true;
            opt.inputPath = arg.substr(6);
        } else if (arg[0] == '-') {
            std::cerr << "Error: Unknown option '" << arg << "'. Use -h / --help for usage.\n";
            return false;
        } else {
            if (opt.inputPath.empty()) {
                opt.inputPath = arg;
            } else {
                std::cerr << "Error: Unexpected additional positional argument '" << arg << "'.\n";
                return false;
            }
        }
    }

    return true;
}

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    CliOptions opt;
    if (!ParseCommandLine(argc, argv, opt)) {
        return 1;
    }

    if (opt.showHelp) {
        PrintHelp();
        return 0;
    }

    if (opt.showVersion) {
        PrintVersion();
        return 0;
    }

    if (opt.inputPath.empty()) {
        std::cerr << "Error: No input file or directory specified.\n"
                  << "Try '" << PROGRAM_NAME << " --help' for more information.\n";
        return 1;
    }

    std::error_code ec;
    if (!fs::exists(opt.inputPath, ec)) {
        std::cerr << "Error: Input path does not exist: " << opt.inputPath.string() << "\n";
        return 1;
    }

    if (fs::is_directory(opt.inputPath, ec)) {
        opt.isDirMode = true;
    }

    // Single File Mode
    if (!opt.isDirMode) {
        auto start = std::chrono::high_resolution_clock::now();
        ConvertResult res = ProcessFile(opt.inputPath, opt.outputTarget, opt.mode, opt.dryRun);
        auto end = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();

        if (!res.success) {
            std::cerr << "[ERROR] " << res.inputPath.string() << ": " << res.errorMessage << "\n";
            return 1;
        }

        if (opt.dryRun) {
            std::cout << "[DRY-RUN] " << res.inputPath.string() << "\n"
                      << "  Would write: " << res.outputPath.string() << " (" << res.outSize << " bytes)\n"
                      << "  Processed in " << std::fixed << std::setprecision(2) << elapsedMs << " ms\n";
        } else {
            std::cout << "[SUCCESS] " << res.inputPath.filename().string()
                      << " -> " << res.outputPath.filename().string()
                      << " (" << res.outSize << " bytes in " << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";
        }

        return 0;
    }

    // Directory Mode
    std::vector<fs::path> targetFiles;
    for (const auto& entry : fs::directory_iterator(opt.inputPath)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (opt.mode == ConvertMode::IniToReg && ext == ".ini") {
            targetFiles.push_back(entry.path());
        } else if (opt.mode == ConvertMode::RegToIni && ext == ".reg") {
            targetFiles.push_back(entry.path());
        } else if (opt.mode == ConvertMode::Auto && (ext == ".ini" || ext == ".reg")) {
            targetFiles.push_back(entry.path());
        }
    }

    std::sort(targetFiles.begin(), targetFiles.end());

    if (targetFiles.empty()) {
        std::cout << "No matching files found in directory: " << opt.inputPath.string() << "\n";
        return 0;
    }

    std::cout << "Found " << targetFiles.size() << " file(s) in " << opt.inputPath.string() << "\n";

    unsigned int threadCount = 1;
    if (opt.multiThread) {
        unsigned int hw = std::thread::hardware_concurrency();
        threadCount = std::max(1u, hw > 0 ? hw : 1u);
        threadCount = std::min<unsigned int>(threadCount, static_cast<unsigned int>(targetFiles.size()));
        std::cout << "Using " << threadCount << " worker thread(s) for parallel processing.\n";
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    std::atomic<size_t> fileIndex{0};
    std::atomic<size_t> successCount{0};
    std::atomic<size_t> failCount{0};
    std::mutex printMutex;

    auto workerFunc = [&]() {
        while (true) {
            size_t idx = fileIndex.fetch_add(1);
            if (idx >= targetFiles.size()) break;

            const fs::path& path = targetFiles[idx];
            ConvertResult res = ProcessFile(path, opt.outputTarget, opt.mode, opt.dryRun);

            std::lock_guard<std::mutex> lock(printMutex);
            if (res.success) {
                successCount++;
                if (opt.dryRun) {
                    std::cout << "[DRY-RUN " << (idx + 1) << "/" << targetFiles.size() << "] "
                              << path.filename().string() << " -> " << res.outputPath.filename().string()
                              << " (" << res.outSize << " B)\n";
                } else {
                    std::cout << "[" << (idx + 1) << "/" << targetFiles.size() << "] Converted "
                              << path.filename().string() << " -> " << res.outputPath.filename().string() << "\n";
                }
            } else {
                failCount++;
                std::cerr << "[ERROR " << (idx + 1) << "/" << targetFiles.size() << "] "
                          << path.filename().string() << ": " << res.errorMessage << "\n";
            }
        }
    };

    if (threadCount > 1) {
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (unsigned int i = 0; i < threadCount; ++i) {
            workers.emplace_back(workerFunc);
        }
        for (auto& w : workers) {
            w.join();
        }
    } else {
        workerFunc();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    std::cout << "\nFinished in " << std::fixed << std::setprecision(2) << totalMs << " ms.\n"
              << "Total: " << targetFiles.size()
              << ", Succeeded: " << successCount.load()
              << ", Failed: " << failCount.load() << "\n";

    return (failCount.load() == 0) ? 0 : 1;
}
