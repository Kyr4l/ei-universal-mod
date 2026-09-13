/**
 * ============================================================================
 * um-mobdumper - High-Performance Evil Islands .mob File Dumper
 * ============================================================================
 * 
 * Description:
 *   Optimized CLI tool to dump Evil Islands binary .mob (Map Object) files into:
 *     1. An EIS (Evil Islands Script) file containing the decrypted mission script.
 *     2. A YAML file containing structured map objects, units, logic, and assets.
 * 
 * Features:
 *   - Fast single-pass binary parsing in native C++ (Linux & Windows compatible).
 *   - Multithreaded batch processing for directories of .mob files (-m / --multi).
 *   - Safe read-only file handling (never modifies input .mob files).
 *   - Support for custom output directory (-o / --output).
 *   - Dry-run simulation mode (--dry-run).
 *   - Robust CP1251 (Windows Cyrillic) to UTF-8 string conversion.
 * 
 * Usage:
 *   um-mobdump [options] <path/to/mobfile.mob>
 *   um-mobdump [options] -d <path/to/directory>
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
#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>

namespace fs = std::filesystem;

// Program version constant
static constexpr const char* PROGRAM_VERSION = "0.1";
static constexpr const char* PROGRAM_NAME = "um-mobdump";

// ============================================================================
// Node Types and Magic Dictionary
// ============================================================================

enum class NodeType {
    Record,          // Container node containing child nodes
    Null,            // Empty / marker node with no stored data
    Dword,           // 32-bit unsigned integer (uint32_t)
    Byte,            // 8-bit unsigned integer (uint8_t)
    Float,           // 32-bit IEEE-754 floating point (float)
    String,          // Null-padded or fixed-length string
    Quaternion,      // 4 floats (X, Y, Z, W)
    Rectangle,       // 4 floats (X1, Y1, X2, Y2)
    Plot,            // 3 floats (X, Y, Z coordinates)
    StringArray,     // Array of counted length-prefixed strings
    UnitStats,       // Array of 43 uint32_t unit attributes
    Diplomacy,       // Array of 1024 int32_t diplomacy matrix entries
    StringEncrypted, // Script encrypted with MSVC LCG XOR cipher
    LeverStats,      // Array of 3 int32_t lever statistics
    Unknown          // Fallback / unparsed blob (outputs "NOT_NOW")
};

struct MagicDef {
    const char* name;
    NodeType type;
};

// Complete dictionary of Evil Islands .mob magic type IDs
static const std::unordered_map<uint32_t, MagicDef> kMagicTable = {
    { 4294967295u, { "UNKNOWN",                 NodeType::Unknown } },
    { 0u,          { "ROOT",                    NodeType::Record } },
    { 43984u,      { "WORLD_SET",               NodeType::Record } },
    { 45072u,      { "OBJ_DEF_LOGIC",           NodeType::Null } },
    { 53248u,      { "PR_OBJECTDBFILE",         NodeType::Null } },
    { 57346u,      { "DIR_NAME",                NodeType::String } },
    { 3722304977u, { "DIPLOMATION",             NodeType::Record } },
    { 43985u,      { "WS_WIND_DIR",             NodeType::Plot } },
    { 45056u,      { "OBJECTSECTION",           NodeType::Record } },
    { 45066u,      { "OBJROTATION",             NodeType::Quaternion } },
    { 45073u,      { "OBJ_PLAYER",              NodeType::Byte } },
    { 57347u,      { "DIR_NINST",               NodeType::Dword } },
    { 3722304978u, { "DIPLOMATION_FOF",         NodeType::Diplomacy } },
    { 40960u,      { "OBJECTDBFILE",            NodeType::Record } },
    { 43520u,      { "LIGHT_SECTION",           NodeType::Null } },
    { 43986u,      { "WS_WIND_STR",             NodeType::Float } },
    { 45057u,      { "OBJECT",                  NodeType::Record } },
    { 45067u,      { "OBJTEXTURE",              NodeType::Null } },
    { 45074u,      { "OBJ_PARENT_ID",           NodeType::Dword } },
    { 52224u,      { "SOUND_SECTION",           NodeType::Null } },
    { 52234u,      { "SOUND_RESNAME",           NodeType::StringArray } },
    { 56576u,      { "PARTICL_SECTION",         NodeType::Null } },
    { 57348u,      { "DIR_PARENT_FOLDER",       NodeType::Dword } },
    { 65280u,      { "SEC_RANGE",               NodeType::Record } },
    { 3722304979u, { "DIPLOMATION_PL_NAMES",    NodeType::StringArray } },
    { 43521u,      { "LIGHT",                   NodeType::Record } },
    { 43987u,      { "WS_TIME",                 NodeType::Float } },
    { 45058u,      { "NID",                     NodeType::Dword } },
    { 45068u,      { "OBJCOMPLECTION",          NodeType::Plot } },
    { 45075u,      { "OBJ_USE_IN_SCRIPT",       NodeType::Byte } },
    { 52225u,      { "SOUND",                   NodeType::Record } },
    { 52235u,      { "SOUND_RANGE2",            NodeType::Dword } },
    { 56577u,      { "PARTICL",                 NodeType::Record } },
    { 57349u,      { "DIR_TYPE",                NodeType::Byte } },
    { 65281u,      { "MAIN_RANGE",              NodeType::Record } },
    { 7696u,       { "VSS_BS_COMMANDS",         NodeType::StringArray } },
    { 43522u,      { "LIGHT_RANGE",             NodeType::Float } },
    { 43988u,      { "WS_AMBIENT",              NodeType::Float } },
    { 45059u,      { "OBJTYPE",                 NodeType::Dword } },
    { 45069u,      { "OBJBODYPARTS",            NodeType::StringArray } },
    { 45076u,      { "OBJ_IS_SHADOW",           NodeType::Byte } },
    { 52226u,      { "SOUND_ID",                NodeType::Dword } },
    { 56578u,      { "PARTICL_ID",              NodeType::Dword } },
    { 65282u,      { "RANGE",                   NodeType::Record } },
    { 7680u,       { "VSS_SECTION",             NodeType::Record } },
    { 7690u,       { "VSS_ISSTART",             NodeType::Byte } },
    { 7697u,       { "VSS_CUSTOM_SRIPT",        NodeType::String } },
    { 43523u,      { "LIGHT_NAME",              NodeType::String } },
    { 43989u,      { "WS_SUN_LIGHT",            NodeType::Float } },
    { 45060u,      { "OBJNAME",                 NodeType::String } },
    { 45070u,      { "PARENTTEMPLATE",          NodeType::String } },
    { 45077u,      { "OBJ_R",                   NodeType::Null } },
    { 52227u,      { "SOUND_POSITION",          NodeType::Plot } },
    { 52237u,      { "SOUND_AMBIENT",           NodeType::Byte } },
    { 56579u,      { "PARTICL_POSITION",        NodeType::Plot } },
    { 3149594624u, { "UNIT",                    NodeType::Record } },
    { 3149594634u, { "UNIT_NEED_IMPORT",        NodeType::Byte } },
    { 7681u,       { "VSS_TRIGER",              NodeType::Record } },
    { 7691u,       { "VSS_LINK",                NodeType::Record } },
    { 43524u,      { "LIGHT_POSITION",          NodeType::Plot } },
    { 45061u,      { "OBJINDEX",                NodeType::Null } },
    { 45071u,      { "OBJCOMMENTS",             NodeType::String } },
    { 45078u,      { "OBJ_QUEST_INFO",          NodeType::String } },
    { 52228u,      { "SOUND_RANGE",             NodeType::Dword } },
    { 52238u,      { "SOUND_IS_MUSIC",          NodeType::Byte } },
    { 56580u,      { "PARTICL_COMMENTS",        NodeType::String } },
    { 3148546048u, { "MAGIC_TRAP",              NodeType::Record } },
    { 3149594625u, { "UNIT_R",                  NodeType::Null } },
    { 3149660160u, { "UNIT_LOGIC",              NodeType::Record } },
    { 3149660170u, { "UNIT_LOGIC_WAIT",         NodeType::Float } },
    { 7682u,       { "VSS_CHECK",               NodeType::Record } },
    { 7692u,       { "VSS_GROUP",               NodeType::String } },
    { 43525u,      { "LIGHT_ID",                NodeType::Dword } },
    { 45062u,      { "OBJTEMPLATE",             NodeType::String } },
    { 52229u,      { "SOUND_NAME",              NodeType::String } },
    { 56581u,      { "PARTICL_NAME",            NodeType::String } },
    { 65285u,      { "MIN_ID",                  NodeType::Dword } },
    { 3148546049u, { "MT_DIPLOMACY",            NodeType::Dword } },
    { 3148611584u, { "LEVER",                   NodeType::Record } },
    { 3149594626u, { "UNIT_PROTOTYPE",          NodeType::String } },
    { 3149660161u, { "UNIT_LOGIC_AGRESSIV",     NodeType::Null } },
    { 3149660171u, { "UNIT_LOGIC_ALARM_CONDITION", NodeType::Byte } },
    { 3149725696u, { "GUARD_PT",                NodeType::Record } },
    { 7683u,       { "VSS_PATH",                NodeType::Record } },
    { 7693u,       { "VSS_IS_USE_GROUP",        NodeType::Byte } },
    { 43526u,      { "LIGHT_SHADOW",            NodeType::Byte } },
    { 45063u,      { "OBJPRIMTXTR",             NodeType::String } },
    { 52230u,      { "SOUND_MIN",               NodeType::Dword } },
    { 56582u,      { "PARTICL_TYPE",            NodeType::Dword } },
    { 65286u,      { "MAX_ID",                  NodeType::Dword } },
    { 3148546050u, { "MT_SPELL",                NodeType::String } },
    { 3148611585u, { "LEVER_SCIENCE_STATS",     NodeType::Null } },
    { 3149594627u, { "UNIT_ITEMS",              NodeType::Null } },
    { 3149660162u, { "UNIT_LOGIC_CYCLIC",       NodeType::Byte } },
    { 3149660172u, { "UNIT_LOGIC_HELP",         NodeType::Float } },
    { 3149725697u, { "GUARD_PT_POSITION",       NodeType::Plot } },
    { 3149791232u, { "ACTION_PT",               NodeType::Record } },
    { 7684u,       { "VSS_ID",                  NodeType::Dword } },
    { 7694u,       { "VSS_VARIABLE",            NodeType::Record } },
    { 43527u,      { "LIGHT_COLOR",             NodeType::Plot } },
    { 45064u,      { "OBJSECTXTR",              NodeType::String } },
    { 52231u,      { "SOUND_MAX",               NodeType::Dword } },
    { 56583u,      { "PARTICL_SCALE",           NodeType::Float } },
    { 826366246u,  { "AIGRAPH",                 NodeType::Unknown } },
    { 3148546051u, { "MT_AREAS",                NodeType::Unknown } },
    { 3148611586u, { "LEVER_CUR_STATE",         NodeType::Byte } },
    { 3149594628u, { "UNIT_STATS",              NodeType::UnitStats } },
    { 3149660163u, { "UNIT_LOGIC_MODEL",        NodeType::Dword } },
    { 3149660173u, { "UNIT_LOGIC_ALWAYS_ACTIVE", NodeType::Byte } },
    { 3149725698u, { "GUARD_PT_ACTION",         NodeType::Null } },
    { 3149791233u, { "ACTION_PT_LOOK_PT",       NodeType::Plot } },
    { 3149856768u, { "TORCH",                   NodeType::Record } },
    { 7685u,       { "VSS_RECT",                NodeType::Rectangle } },
    { 7695u,       { "VSS_BS_CHECK",            NodeType::StringArray } },
    { 43528u,      { "LIGHT_COMMENTS",          NodeType::String } },
    { 45065u,      { "OBJPOSITION",             NodeType::Plot } },
    { 52232u,      { "SOUND_COMMENTS",          NodeType::String } },
    { 3148546052u, { "MT_TARGETS",              NodeType::Unknown } },
    { 3148611587u, { "LEVER_TOTAL_STATE",       NodeType::Byte } },
    { 3149594629u, { "UNIT_QUEST_ITEMS",        NodeType::StringArray } },
    { 3149660164u, { "UNIT_LOGIC_GUARD_R",      NodeType::Float } },
    { 3149660174u, { "UNIT_LOGIC_AGRESSION_MODE", NodeType::Byte } },
    { 3149791234u, { "ACTION_PT_WAIT_SEG",      NodeType::Dword } },
    { 3149856769u, { "TORCH_STRENGHT",          NodeType::Float } },
    { 7686u,       { "VSS_SRC_ID",              NodeType::Dword } },
    { 52233u,      { "SOUND_VOLUME",            NodeType::Null } },
    { 3148546053u, { "MT_CAST_INTERVAL",        NodeType::Dword } },
    { 3148611588u, { "LEVER_IS_CYCLED",         NodeType::Byte } },
    { 3149594630u, { "UNIT_QUICK_ITEMS",        NodeType::StringArray } },
    { 3149660165u, { "UNIT_LOGIC_GUARD_PT",     NodeType::Plot } },
    { 3149791235u, { "ACTION_PT_TURN_SPEED",    NodeType::Dword } },
    { 3149856770u, { "TORCH_PTLINK",            NodeType::Plot } },
    { 7687u,       { "VSS_DST_ID",              NodeType::Dword } },
    { 3148611589u, { "LEVER_CAST_ONCE",         NodeType::Byte } },
    { 3149594631u, { "UNIT_SPELLS",             NodeType::StringArray } },
    { 3149660166u, { "UNIT_LOGIC_NALARM",       NodeType::Byte } },
    { 3149791236u, { "ACTION_PT_FLAGS",         NodeType::Byte } },
    { 3149856771u, { "TORCH_SOUND",             NodeType::String } },
    { 7688u,       { "VSS_TITLE",               NodeType::String } },
    { 3148611590u, { "LEVER_SCIENCE_STATS_NEW", NodeType::LeverStats } },
    { 3149594632u, { "UNIT_WEAPONS",            NodeType::StringArray } },
    { 3149660167u, { "UNIT_LOGIC_USE",          NodeType::Byte } },
    { 7689u,       { "VSS_COMMANDS",            NodeType::String } },
    { 61440u,      { "DIRICTORY_ELEMENTS",      NodeType::Record } },
    { 3148611591u, { "LEVER_IS_DOOR",           NodeType::Byte } },
    { 3149594633u, { "UNIT_ARMORS",             NodeType::StringArray } },
    { 3149660168u, { "UNIT_LOGIC_REVENGE",      NodeType::Null } },
    { 57344u,      { "DIRICTORY",               NodeType::Record } },
    { 2899242186u, { "SS_TEXT_OLD",             NodeType::String } },
    { 3148611592u, { "LEVER_RECALC_GRAPH",      NodeType::Byte } },
    { 3149660169u, { "UNIT_LOGIC_FEAR",         NodeType::Null } },
    { 49152u,      { "SC_OBJECTDBFILE",         NodeType::Null } },
    { 57345u,      { "FOLDER",                  NodeType::Record } },
    { 2899242187u, { "SS_TEXT",                  NodeType::StringEncrypted } }
};

// ============================================================================
// CP1251 (Windows Cyrillic) to UTF-8 Conversion Table
// ============================================================================

// Unicode code points for Windows-1251 bytes 0x80..0xFF
static const uint32_t kCp1251ToUnicode[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, // 0x80 - 0x87
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, // 0x88 - 0x8F
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 0x90 - 0x97
    0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, // 0x98 - 0x9F
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, // 0xA0 - 0xA7
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, // 0xA8 - 0xAF
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, // 0xB0 - 0xB7
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, // 0xB8 - 0xBF
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, // 0xC0 - 0xC7
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, // 0xC8 - 0xCF
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, // 0xD0 - 0xD7
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, // 0xD8 - 0xDF
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, // 0xE0 - 0xE7
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F, // 0xE8 - 0xEF
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, // 0xF0 - 0xF7
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F  // 0xF8 - 0xFF
};

/**
 * Appends a Unicode code point encoded in UTF-8 to the target string buffer.
 */
static inline void AppendUtf8CodePoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/**
 * Decodes raw bytes from CP1251 to standard UTF-8 string, filtering null bytes.
 */
static std::string DecodeCp1251String(const uint8_t* data, size_t length) {
    std::string result;
    result.reserve(length + length / 4);
    for (size_t i = 0; i < length; ++i) {
        uint8_t b = data[i];
        if (b == 0) {
            continue; // Skip embedded null characters
        }
        if (b < 0x80) {
            result.push_back(static_cast<char>(b));
        } else {
            AppendUtf8CodePoint(result, kCp1251ToUnicode[b - 0x80]);
        }
    }
    return result;
}

// ============================================================================
// Script Decryption (MSVC LCG Linear Congruential Generator XOR Cipher)
// ============================================================================

/**
 * Decrypts Evil Islands mission scripts (SS_TEXT node).
 * The encryption uses MSVC's standard linear congruential generator:
 *   key = key * 214013 + 2531011 (modulo 2^32)
 * Each ciphertext byte is XORed with (key >> 16) & 0xFF.
 */
static std::string DecryptScript(const uint8_t* payload, size_t payloadLen) {
    if (payloadLen < 4) return "";
    
    // Read 32-bit initial key (little endian)
    uint32_t key = static_cast<uint32_t>(payload[0]) |
                  (static_cast<uint32_t>(payload[1]) << 8) |
                  (static_cast<uint32_t>(payload[2]) << 16) |
                  (static_cast<uint32_t>(payload[3]) << 24);

    size_t cipherLen = payloadLen - 4;
    const uint8_t* cipherText = payload + 4;

    std::vector<uint8_t> decryptedBytes(cipherLen);
    for (size_t i = 0; i < cipherLen; ++i) {
        key = key * 214013u + 2531011u;
        uint8_t mask = static_cast<uint8_t>((key >> 16) & 0xFF);
        decryptedBytes[i] = cipherText[i] ^ mask;
    }

    // Decode decrypted bytes using CP1251 to UTF-8
    return DecodeCp1251String(decryptedBytes.data(), decryptedBytes.size());
}

// ============================================================================
// Formatting and Escaping Utilities
// ============================================================================

/**
 * Escapes YAML string value:
 *   - Converts \r\n to literal \n
 *   - Converts \n to literal \n
 *   - Converts " to literal \"
 */
static std::string EscapeYamlString(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                out.append("\\n");
                ++i;
            } else {
                out.append("\\n");
            }
        } else if (c == '\n') {
            out.append("\\n");
        } else if (c == '"') {
            out.append("\\\"");
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/**
 * Appends N spaces corresponding to the YAML indentation level (2 spaces per level).
 */
static inline void AppendIndent(std::string& out, int level) {
    out.append(level * 2, ' ');
}

/**
 * Formats a 32-bit float matching compact floating point representation.
 */
static inline std::string FormatFloat(float val) {
    // Check for exact zero
    if (val == 0.0f) return "0.0";
    
    // Convert to double for consistent precision representation
    double dval = static_cast<double>(val);
    char buf[64];
    // Format with maximum significant digits
    int len = snprintf(buf, sizeof(buf), "%.17g", dval);
    
    // If output is plain integer without decimal point or exponent, append .0
    bool hasDecimalOrExp = false;
    for (int i = 0; i < len; ++i) {
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') {
            hasDecimalOrExp = true;
            break;
        }
    }
    if (!hasDecimalOrExp) {
        snprintf(buf + len, sizeof(buf) - len, ".0");
    }
    return std::string(buf);
}

// ============================================================================
// Binary Readers (Safe Buffer Access)
// ============================================================================

static inline uint8_t ReadUint8(const uint8_t* ptr) {
    return *ptr;
}

static inline int8_t ReadInt8(const uint8_t* ptr) {
    return static_cast<int8_t>(*ptr);
}

static inline uint32_t ReadUint32LE(const uint8_t* ptr) {
    return static_cast<uint32_t>(ptr[0]) |
          (static_cast<uint32_t>(ptr[1]) << 8) |
          (static_cast<uint32_t>(ptr[2]) << 16) |
          (static_cast<uint32_t>(ptr[3]) << 24);
}

static inline int32_t ReadInt32LE(const uint8_t* ptr) {
    uint32_t u = ReadUint32LE(ptr);
    return static_cast<int32_t>(u);
}

static inline float ReadFloatLE(const uint8_t* ptr) {
    uint32_t u = ReadUint32LE(ptr);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// ============================================================================
// Recursive MOB Node Parser & YAML/EIS Serializer
// ============================================================================

/**
 * Recursively parses .mob nodes within the byte slice [offset, endOffset).
 * Directly formats YAML into yamlOut and extracts script into eisScript.
 */
static bool ParseMobNode(
    const uint8_t* data,
    size_t offset,
    size_t endOffset,
    int level,
    std::string& yamlOut,
    std::string& eisScript)
{
    while (offset < endOffset) {
        // Validate header bounds (each node has 4-byte type + 4-byte length)
        if (offset + 8 > endOffset) {
            return false;
        }

        uint32_t magic = ReadUint32LE(data + offset);
        uint32_t nodeLen = ReadUint32LE(data + offset + 4);

        if (nodeLen < 8 || offset + nodeLen > endOffset) {
            // Node extends past available data or has invalid declared length
            return false;
        }

        const uint8_t* payload = data + offset + 8;
        size_t payloadLen = nodeLen - 8;

        // Resolve magic definition
        auto it = kMagicTable.find(magic);
        const char* nodeName = (it != kMagicTable.end()) ? it->second.name : "UNKNOWN";
        NodeType nodeType = (it != kMagicTable.end()) ? it->second.type : NodeType::Unknown;

        switch (nodeType) {
            case NodeType::Record: {
                AppendIndent(yamlOut, level);
                yamlOut.append(nodeName);
                yamlOut.append(":\n");
                // Parse nested child records
                ParseMobNode(data, offset + 8, offset + nodeLen, level + 1, yamlOut, eisScript);
                break;
            }

            case NodeType::Null: {
                // Marker nodes have no YAML content
                break;
            }

            case NodeType::Dword: {
                if (nodeLen == 12) {
                    uint32_t val = ReadUint32LE(payload);
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(": ");
                    yamlOut.append(std::to_string(val));
                    yamlOut.append("\n");
                }
                break;
            }

            case NodeType::Byte: {
                if (nodeLen == 9) {
                    uint8_t val = ReadUint8(payload);
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(": ");
                    yamlOut.append(std::to_string(static_cast<unsigned int>(val)));
                    yamlOut.append("\n");
                }
                break;
            }

            case NodeType::Float: {
                if (nodeLen == 12) {
                    float val = ReadFloatLE(payload);
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(": ");
                    yamlOut.append(FormatFloat(val));
                    yamlOut.append("\n");
                }
                break;
            }

            case NodeType::String: {
                std::string str = DecodeCp1251String(payload, payloadLen);
                if (std::strcmp(nodeName, "SS_TEXT_OLD") == 0) {
                    // Extract legacy plaintext script
                    eisScript = str;
                } else {
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(": \"");
                    yamlOut.append(EscapeYamlString(str));
                    yamlOut.append("\"\n");
                }
                break;
            }

            case NodeType::Quaternion:
            case NodeType::Rectangle: {
                if (nodeLen == 24) { // 4 floats = 16 bytes payload
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(":\n");
                    for (int i = 0; i < 4; ++i) {
                        float f = ReadFloatLE(payload + i * 4);
                        AppendIndent(yamlOut, level + 1);
                        yamlOut.append("- ");
                        yamlOut.append(FormatFloat(f));
                        yamlOut.append("\n");
                    }
                }
                break;
            }

            case NodeType::Plot: {
                if (nodeLen == 20) { // 3 floats = 12 bytes payload
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(":\n");
                    for (int i = 0; i < 3; ++i) {
                        float f = ReadFloatLE(payload + i * 4);
                        AppendIndent(yamlOut, level + 1);
                        yamlOut.append("- ");
                        yamlOut.append(FormatFloat(f));
                        yamlOut.append("\n");
                    }
                }
                break;
            }

            case NodeType::StringArray: {
                if (payloadLen >= 4) {
                    uint32_t records = ReadUint32LE(payload);
                    if (records == 0) {
                        AppendIndent(yamlOut, level);
                        yamlOut.append(nodeName);
                        yamlOut.append(": None\n");
                    } else {
                        AppendIndent(yamlOut, level);
                        yamlOut.append(nodeName);
                        yamlOut.append(":\n");

                        size_t arrPos = 4;
                        for (uint32_t r = 0; r < records && arrPos + 8 <= payloadLen; ++r) {
                            // uint32_t elemType = ReadUint32LE(payload + arrPos);
                            uint32_t strLen = ReadUint32LE(payload + arrPos + 4);
                            arrPos += 8;
                            if (strLen >= 8 && arrPos + (strLen - 8) <= payloadLen) {
                                std::string itemStr = DecodeCp1251String(payload + arrPos, strLen - 8);
                                arrPos += (strLen - 8);
                                AppendIndent(yamlOut, level + 1);
                                yamlOut.append("- \"");
                                yamlOut.append(EscapeYamlString(itemStr));
                                yamlOut.append("\"\n");
                            } else {
                                break;
                            }
                        }
                    }
                }
                break;
            }

            case NodeType::UnitStats: {
                if (nodeLen == 180) { // 43 uint32s = 172 bytes payload
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(":\n");
                    for (int i = 0; i < 43; ++i) {
                        uint32_t stat = ReadUint32LE(payload + i * 4);
                        AppendIndent(yamlOut, level + 1);
                        yamlOut.append("- ");
                        yamlOut.append(std::to_string(stat));
                        yamlOut.append("\n");
                    }
                }
                break;
            }

            case NodeType::Diplomacy: {
                if (nodeLen == 4104) { // 1024 int32s = 4096 bytes payload
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(":\n");
                    for (int i = 0; i < 1024; ++i) {
                        int32_t dip = ReadInt32LE(payload + i * 4);
                        AppendIndent(yamlOut, level + 1);
                        yamlOut.append("- ");
                        yamlOut.append(std::to_string(dip));
                        yamlOut.append("\n");
                    }
                }
                break;
            }

            case NodeType::StringEncrypted: {
                std::string decrypted = DecryptScript(payload, payloadLen);
                if (std::strcmp(nodeName, "SS_TEXT") == 0) {
                    // Extract mission script
                    eisScript = decrypted;
                } else {
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(": \"");
                    yamlOut.append(EscapeYamlString(decrypted));
                    yamlOut.append("\"\n");
                }
                break;
            }

            case NodeType::LeverStats: {
                if (nodeLen == 20) { // 3 int32s = 12 bytes payload
                    AppendIndent(yamlOut, level);
                    yamlOut.append(nodeName);
                    yamlOut.append(":\n");
                    for (int i = 0; i < 3; ++i) {
                        int32_t ls = ReadInt32LE(payload + i * 4);
                        AppendIndent(yamlOut, level + 1);
                        yamlOut.append("- ");
                        yamlOut.append(std::to_string(ls));
                        yamlOut.append("\n");
                    }
                }
                break;
            }

            case NodeType::Unknown:
            default: {
                AppendIndent(yamlOut, level);
                yamlOut.append(nodeName);
                yamlOut.append(": \"NOT_NOW\"\n");
                break;
            }
        }

        offset += nodeLen;
    }

    return true;
}

// ============================================================================
// Single File Processing
// ============================================================================

struct DumpResult {
    bool success = false;
    fs::path mobPath;
    fs::path yamlPath;
    fs::path eisPath;
    size_t fileSize = 0;
    size_t yamlSize = 0;
    size_t scriptSize = 0;
    std::string errorMessage;
};

/**
 * Dumps a single .mob file to .yaml and .eis files.
 * If dryRun is true, parsing occurs in memory without writing files to disk.
 */
static DumpResult ProcessMobFile(
    const fs::path& mobPath,
    const fs::path& outputDir,
    bool dryRun)
{
    DumpResult res;
    res.mobPath = mobPath;

    // Determine output directory: user specified or same folder as mobPath
    fs::path targetDir = outputDir.empty() ? mobPath.parent_path() : outputDir;
    if (targetDir.empty()) {
        targetDir = ".";
    }

    // Determine target filenames (same stem as mob file)
    std::string stem = mobPath.stem().string();
    res.yamlPath = targetDir / (stem + ".yaml");
    res.eisPath = targetDir / (stem + ".eis");

    // Open .mob file strictly for READ ONLY
    std::ifstream file(mobPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        res.errorMessage = "Failed to open input file for reading: " + mobPath.string();
        return res;
    }

    std::streamsize size = file.tellg();
    if (size < 8) {
        res.errorMessage = "File is too small to be a valid .mob file (less than 8 bytes)";
        return res;
    }
    res.fileSize = static_cast<size_t>(size);

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(res.fileSize);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        res.errorMessage = "Failed to read file contents into memory: " + mobPath.string();
        return res;
    }
    file.close();

    // Verify root magic (0x0000A000 = 40960 = OBJECTDBFILE)
    uint32_t rootMagic = ReadUint32LE(buffer.data());
    if (rootMagic != 40960u) {
        res.errorMessage = "Incorrect magic header (expected OBJECTDBFILE 40960 / 0x0000A000)";
        return res;
    }

    // Parse and generate YAML / EIS
    std::string yamlContent;
    std::string scriptContent;
    yamlContent.reserve(res.fileSize * 2);

    if (!ParseMobNode(buffer.data(), 0, buffer.size(), 0, yamlContent, scriptContent)) {
        res.errorMessage = "Warning: Reached unexpected end of file or malformed node structure";
        // Continue to write whatever was successfully dumped
    }

    res.yamlSize = yamlContent.size();
    res.scriptSize = scriptContent.size();

    // If dry run, do not write files
    if (dryRun) {
        res.success = true;
        return res;
    }

    // Ensure destination directory exists
    std::error_code ec;
    if (!fs::exists(targetDir, ec)) {
        fs::create_directories(targetDir, ec);
        if (ec) {
            res.errorMessage = "Failed to create output directory: " + targetDir.string() + " (" + ec.message() + ")";
            return res;
        }
    }

    // Write YAML file
    {
        std::ofstream yamlFile(res.yamlPath, std::ios::binary);
        if (!yamlFile.is_open()) {
            res.errorMessage = "Failed to open output YAML file for writing: " + res.yamlPath.string();
            return res;
        }
        yamlFile.write(yamlContent.data(), yamlContent.size());
        if (!yamlFile) {
            res.errorMessage = "Failed to complete writing YAML file: " + res.yamlPath.string();
            return res;
        }
    }

    // Write EIS script file
    {
        std::ofstream eisFile(res.eisPath, std::ios::binary);
        if (!eisFile.is_open()) {
            res.errorMessage = "Failed to open output EIS file for writing: " + res.eisPath.string();
            return res;
        }
        eisFile.write(scriptContent.data(), scriptContent.size());
        if (!eisFile) {
            res.errorMessage = "Failed to complete writing EIS file: " + res.eisPath.string();
            return res;
        }
    }

    res.success = true;
    return res;
}

// ============================================================================
// CLI Argument Parsing & Multi-Threading Engine
// ============================================================================

struct CliOptions {
    bool showHelp = false;
    bool showVersion = false;
    bool multiThread = false;
    bool isDirMode = false;
    bool dryRun = false;
    fs::path inputPath;
    fs::path outputDir;
};

static void PrintVersion() {
    std::cout << PROGRAM_NAME << " version " << PROGRAM_VERSION << "\n";
}

static void PrintHelp() {
    std::cout << "um-mobdump - High-Performance Evil Islands .mob File Dumper\n\n"
              << "Usage:\n"
              << "  um-mobdump [options] <path/to/mobfile.mob>\n"
              << "  um-mobdump [options] -d <path/to/directory>\n\n"
              << "Options:\n"
              << "  -o, --output <dir>    Set output directory for .eis and .yaml files\n"
              << "                        (default: same directory as input .mob file)\n"
              << "  -d, --dir <dir>       Process all .mob files in the selected directory\n"
              << "  -m, --multi           Process multiple files in parallel across CPU cores\n"
              << "                        (only active when used together with -d / --dir)\n"
              << "  --dry-run             Show what the program would do without writing anything\n"
              << "  -v, --version         Print program version (" << PROGRAM_VERSION << ")\n"
              << "  -h, --help            Print this help message\n\n"
              << "Examples:\n"
              << "  um-mobdump maps/zone1.mob\n"
              << "  um-mobdump -o ./dumped maps/zone1.mob\n"
              << "  um-mobdump -d ./maps -o ./output -m\n"
              << "  um-mobdump -d ./maps --dry-run\n";
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
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                opt.outputDir = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires a directory argument.\n";
                return false;
            }
        } else if (arg.rfind("--output=", 0) == 0) {
            opt.outputDir = arg.substr(9);
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
            // Positional argument
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

/**
 * Checks case-insensitively if a path has a .mob extension.
 */
static bool HasMobExtension(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".mob";
}

int main(int argc, char* argv[]) {
    // Fast I/O
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
                  << "Usage: " << PROGRAM_NAME << " [options] <path/to/mobfile.mob>\n"
                  << "Try '" << PROGRAM_NAME << " --help' for more information.\n";
        return 1;
    }

    // Verify input path existence
    std::error_code ec;
    if (!fs::exists(opt.inputPath, ec)) {
        std::cerr << "Error: Input path does not exist: " << opt.inputPath.string() << "\n";
        return 1;
    }

    // Auto-detect directory mode if inputPath is a directory
    if (fs::is_directory(opt.inputPath, ec)) {
        opt.isDirMode = true;
    }

    // ========================================================================
    // Single File Mode
    // ========================================================================
    if (!opt.isDirMode) {
        if (opt.multiThread) {
            std::cout << "[Note] Multi-threading flag (-m/--multi) ignored in single-file mode.\n";
        }

        auto start = std::chrono::high_resolution_clock::now();
        DumpResult res = ProcessMobFile(opt.inputPath, opt.outputDir, opt.dryRun);
        auto end = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();

        if (!res.success) {
            std::cerr << "[ERROR] " << res.mobPath.string() << ": " << res.errorMessage << "\n";
            return 1;
        }

        if (opt.dryRun) {
            std::cout << "[DRY-RUN] " << res.mobPath.string() << "\n"
                      << "  Would write YAML: " << res.yamlPath.string() << " (" << res.yamlSize << " bytes)\n"
                      << "  Would write EIS:  " << res.eisPath.string() << " (" << res.scriptSize << " bytes)\n"
                      << "  Processed in " << std::fixed << std::setprecision(2) << elapsedMs << " ms\n";
        } else {
            std::cout << "[SUCCESS] Dumped " << res.mobPath.filename().string()
                      << " -> " << res.yamlPath.filename().string() << ", " << res.eisPath.filename().string()
                      << " in " << std::fixed << std::setprecision(2) << elapsedMs << " ms\n";
        }

        return 0;
    }

    // ========================================================================
    // Directory Mode
    // ========================================================================
    std::vector<fs::path> mobFiles;
    try {
        for (const auto& entry : fs::directory_iterator(opt.inputPath)) {
            if (entry.is_regular_file() && HasMobExtension(entry.path())) {
                mobFiles.push_back(entry.path());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error scanning directory " << opt.inputPath.string() << ": " << e.what() << "\n";
        return 1;
    }

    // Sort files alphabetically for clean deterministic processing
    std::sort(mobFiles.begin(), mobFiles.end());

    if (mobFiles.empty()) {
        std::cout << "No .mob files found in directory: " << opt.inputPath.string() << "\n";
        return 0;
    }

    std::cout << "Found " << mobFiles.size() << " .mob file(s) in " << opt.inputPath.string() << "\n";

    // Determine thread count
    unsigned int threadCount = 1;
    if (opt.multiThread) {
        unsigned int hw = std::thread::hardware_concurrency();
        threadCount = std::max(1u, hw > 0 ? hw : 1u);
        threadCount = std::min<unsigned int>(threadCount, static_cast<unsigned int>(mobFiles.size()));
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
            if (idx >= mobFiles.size()) break;

            const fs::path& path = mobFiles[idx];
            DumpResult res = ProcessMobFile(path, opt.outputDir, opt.dryRun);

            std::lock_guard<std::mutex> lock(printMutex);
            if (res.success) {
                successCount++;
                if (opt.dryRun) {
                    std::cout << "[DRY-RUN " << (idx + 1) << "/" << mobFiles.size() << "] "
                              << path.filename().string()
                              << " -> YAML: " << res.yamlPath.string() << " (" << res.yamlSize << " B), "
                              << "EIS: " << res.eisPath.string() << " (" << res.scriptSize << " B)\n";
                } else {
                    std::cout << "[" << (idx + 1) << "/" << mobFiles.size() << "] Dumped "
                              << path.filename().string() << "\n";
                }
            } else {
                failCount++;
                std::cerr << "[ERROR " << (idx + 1) << "/" << mobFiles.size() << "] "
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
              << "Total: " << mobFiles.size()
              << ", Succeeded: " << successCount.load()
              << ", Failed: " << failCount.load() << "\n";

    return (failCount.load() == 0) ? 0 : 1;
}
