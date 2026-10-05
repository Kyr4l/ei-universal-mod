// Reads a .mob map file (docs/file-formats/mob-format.md) into what the map editor shows and
// checks: every placed object (world objects, units, levers, torches, magic traps, lights,
// particles, sounds) with its position, rotation and figure, the mission script, and the
// structural problems found on the way. Read-only; no GL.
//
// Every node is [type:4][length:4][payload], length including the 8-byte header. The file is
// OBJECTDBFILE (whose length is often stale), an 8-byte marker, then top-level sections up to a
// ROOT node or the end of the file (the navmesh AIGRAPH usually sits after the root's length).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cp1251.hpp"

namespace mob {

enum : uint32_t {
    kRoot = 0,
    kObjectDbFile = 40960,
    kObjectSection = 45056,
    kObject = 45057,
    kNid = 45058,
    kObjType = 45059,
    kObjName = 45060,
    kObjTemplate = 45062,
    kObjPrimTexture = 45063,
    kObjSecTexture = 45064,
    kObjPosition = 45065,
    kObjRotation = 45066,
    kObjComplection = 45068,
    kObjBodyParts = 45069,
    kParentTemplate = 45070,
    kObjComments = 45071,
    kObjPlayer = 45073,
    kObjParentId = 45074,
    kObjUseInScript = 45075,
    kObjQuestInfo = 45078,
    kLight = 43521,
    kLightRange = 43522,
    kLightName = 43523,
    kLightPosition = 43524,
    kLightId = 43525,
    kLightColor = 43527,
    kSound = 52225,
    kSoundId = 52226,
    kSoundPosition = 52227,
    kSoundRange = 52228,
    kSoundName = 52229,
    kSoundResName = 52234,
    kParticle = 56577,
    kParticleId = 56578,
    kParticlePosition = 56579,
    kParticleName = 56581,
    kParticleType = 56582,
    kParticleScale = 56583,
    kMainRange = 65281,
    kSecRange = 65280,
    kMinId = 65285,
    kMaxId = 65286,
    kUnit = 3149594624u,
    kUnitPrototype = 3149594626u,
    kUnitNeedImport = 3149594634u,
    kUnitStats = 3149594628u,      // 172 bytes: ei_maper's SUnitStat
    kUnitQuestItems = 3149594629u,
    kUnitQuickItems = 3149594630u,
    kUnitSpells = 3149594631u,
    kUnitWeapons = 3149594632u,
    kUnitArmors = 3149594633u,
    kUnitLogic = 3149660160u,
    kLogicCyclic = 3149660162u,
    kLogicModel = 3149660163u,
    kLogicGuardRadius = 3149660164u,
    kLogicGuardPlace = 3149660165u,
    kLogicAlarmCount = 3149660166u,
    kLogicUse = 3149660167u,
    kLogicWait = 3149660170u,
    kLogicAlarmCondition = 3149660171u,
    kLogicHelp = 3149660172u,
    kLogicAlwaysActive = 3149660173u,
    kLogicAggression = 3149660174u,
    kGuardPoint = 3149725696u,
    kGuardPointPosition = 3149725697u,
    kActionPoint = 3149791232u,
    kActionLookPoint = 3149791233u,
    kActionWait = 3149791234u,
    kActionTurnSpeed = 3149791235u,
    kActionFlags = 3149791236u,
    kWorldSet = 43984,
    kWorldWindDir = 43985,
    kWorldWindStr = 43986,
    kWorldTime = 43987,
    kWorldAmbient = 43988,
    kWorldSunLight = 43989,
    kLever = 3148611584u,
    kTorch = 3149856768u,
    kMagicTrap = 3148546048u,
    kMagicTrapSpell = 3148546050u,
    kMagicTrapAreas = 3148546051u,   // [count] then x, y, radius each
    kMagicTrapTargets = 3148546052u, // [count] then x, y each (cast points)
    kScriptOld = 2899242186u, // plain text
    kScript = 2899242187u,    // encrypted
    kAiGraph = 826366246u,
};

enum class Kind { Object, Unit, Lever, Torch, MagicTrap, Light, Particle, Sound };

inline const char* KindName(Kind k) {
    switch (k) {
    case Kind::Object: return "Object";
    case Kind::Unit: return "Unit";
    case Kind::Lever: return "Lever";
    case Kind::Torch: return "Torch";
    case Kind::MagicTrap: return "Magic trap";
    case Kind::Light: return "Light";
    case Kind::Particle: return "Particle";
    case Kind::Sound: return "Sound";
    }
    return "?";
}

// Lights, particles and sounds have no figure; their position is absolute. The others stand on the
// ground: their z is added to the terrain height under them.
// Magic traps stand on the ground like figures but have no 3D model: only areas and cast points.
inline bool HasModel(Kind k) { return k != Kind::MagicTrap && k != Kind::Light && k != Kind::Particle && k != Kind::Sound; }
inline bool HasFigure(Kind k) { return k != Kind::Light && k != Kind::Particle && k != Kind::Sound; }

struct Vec3 { float x = 0, y = 0, z = 0; };

// A unit's behaviour (UNIT_LOGIC), as ei_maper reads it (objects/unit.cpp: CLogic, CPatrolPoint,
// CLookPoint). A unit carries several; the ones with `use` set apply.
struct LookPoint {
    Vec3 position;
    uint32_t wait = 0;       // ACTION_PT_WAIT_SEG
    float turnSpeed = 0;     // ACTION_PT_TURN_SPEED (stored as float bits)
    uint8_t flags = 0;
};
struct PatrolPoint {
    Vec3 position;
    std::vector<LookPoint> looks;
};
struct Logic {
    bool use = false, cyclic = false;
    int model = 0;           // 0 idle, 1 guard (radius), 2 patrol (path), 3 sentry (place), 4 briefing, 5 guard alarm
    float guardRadius = 0;
    Vec3 guardPlace;
    int alarmCount = 0, alarmCondition = 0, alwaysActive = 0, aggression = 0;
    float wait = 0;          // idle time; ei_maper: 15 = 1 second (-1 = none)
    float help = 0;          // radius within which it calls for help
    std::vector<PatrolPoint> patrol;
};

inline const char* LogicModelName(int m) {
    static const char* const names[] = {"idle", "guard (radius)", "patrol (path)", "sentry (place)", "briefing", "guard alarm"};
    return m >= 0 && m <= 5 ? names[m] : "unknown";
}

// One named list of a unit's (weapons, armors, spells, quick items, quest items).
struct ItemList {
    uint32_t type = 0;
    const char* label = "";
    std::vector<std::string> entries; // as stored (cp1251 bytes, NUL stripped)
    bool truncated = false;
    bool badLength = false;
    uint32_t badLengthValue = 0;
    uint32_t declaredCount = 0;
};

struct Object {
    Kind kind = Kind::Object;
    uint32_t id = 0;      // NID, or LIGHT_ID / PARTICL_ID / SOUND_ID
    bool hasId = false;
    std::string name, templ, primTexture, secTexture, parentTemplate, comments, questInfo;
    std::string prototype;     // units
    bool needImport = false;   // units: stats come from the prototype
    std::vector<uint8_t> stats; // units: UNIT_STATS as stored (172 bytes)
    std::string spell;         // magic traps
    std::vector<Vec3> trapAreas;   // magic traps: activation areas (x, y, z = radius)
    std::vector<Vec3> trapTargets; // magic traps: cast points (x, y)
    std::string particleName;  // particles: the effect; sounds: the sound name
    std::vector<std::string> bodyParts, soundFiles;
    Vec3 position;
    float rotation[4] = {1, 0, 0, 0}; // w, x, y, z
    Vec3 complection{1, 1, 1};
    Vec3 color{1, 1, 1};              // lights
    float range = 0.0f;               // lights, sounds
    float scale = 1.0f;               // particles
    int player = -1;
    int type = -1;
    uint32_t parentId = 0;
    bool hasParent = false;
    std::vector<ItemList> lists;      // units
    std::vector<Logic> logics;        // units: behaviours (see Logic)
    size_t offset = 0;                // where its node starts in the file
    // Where the editable fixed-size fields' payloads are in the file (0 = the object has none): edits
    // patch these bytes in place, so the rest of the file stays exactly as it was.
    size_t positionAt = 0, rotationAt = 0, complectionAt = 0, idAt = 0;
};

struct Range { uint32_t min = 0, max = 0; size_t minAt = 0, maxAt = 0; }; // *At: the values' payloads in the file

struct Issue {
    char severity; // 'E' error, 'W' warning, 'I' info
    std::string message;
};

struct File {
    std::string path, fileName; // fileName: without folders
    bool loaded = false;
    std::string error;          // why it could not be read at all
    std::vector<uint8_t> bytes;
    std::vector<Object> objects;
    std::string script;
    bool hasScript = false;
    std::vector<Range> mainRanges, secRanges;
    std::vector<Issue> structure; // problems found while reading
    size_t objectSectionAt = 0;   // the OBJECTSECTION node (0 = none)
    size_t aiGraphBytes = 0;
    size_t aiGraphAt = 0;         // AI_GRAPH's payload (0: none or not understood), see AiCost
    int aiW = 0, aiH = 0;         // its grid: one node per 4 x 4 world units
    // DIPLOMATION: the 32 x 32 table between player groups (0 friend, 1 neutral, 2 enemy) and their names.
    bool hasDiplomacy = false;
    size_t diplomacyAt = 0;              // the table's payload in the file
    std::vector<int32_t> diplomacy;      // row-major, 32 x 32
    std::vector<std::string> diplomacyNames;
    bool hasWorld = false;        // WORLD_SET: the map's time of day and light levels
    float worldTime = 12.0f, worldAmbient = 0.2f, worldSunLight = 0.6f;
    Vec3 worldWindDir;
    float worldWindStr = 0;
};

inline uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline float F32(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }

inline std::string Text(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    size_t nul = s.find('\0');
    if (nul != std::string::npos) s.resize(nul);
    return s;
}

// cp1251 bytes (names, comments) -> UTF-8 for display.
inline std::string Utf8(const std::string& cp1251Text) {
    std::string out;
    for (unsigned char c : cp1251Text) {
        if (c < 0x80) { out += static_cast<char>(c); continue; }
        uint32_t cp = c;
        for (const auto& pair : cp1251::NonAsciiTable()) if (pair.byte == c) { cp = pair.codepoint; break; }
        if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    return out;
}

// MSVC rand() XOR over everything after the 4-byte key; NUL bytes are padding (same as um.dll).
inline std::string DecryptScript(const uint8_t* payload, size_t length) {
    std::string text;
    if (length < 4) return text;
    uint32_t key = U32(payload);
    text.reserve(length - 4);
    for (size_t i = 4; i < length; ++i) {
        key = key * 214013u + 2531011u;
        uint8_t plain = static_cast<uint8_t>(payload[i] ^ ((key >> 16) & 0xFF));
        if (plain) text.push_back(static_cast<char>(plain));
    }
    return text;
}

// [count:4] then count x [type:4][length:4][bytes]; flags a truncated list or a bad entry length.
inline std::vector<std::string> StringArray(const uint8_t* p, size_t n, bool* truncated = nullptr, bool* badLength = nullptr,
                                            uint32_t* badValue = nullptr, uint32_t* declared = nullptr) {
    std::vector<std::string> out;
    if (n < 4) return out;
    uint32_t count = U32(p);
    if (declared) *declared = count;
    size_t pos = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 8 > n) { if (truncated) *truncated = true; break; }
        uint32_t length = U32(p + pos + 4);
        if (length < 8 || pos + length > n) {
            if (badLength) *badLength = true;
            if (badValue) *badValue = length;
            break;
        }
        out.push_back(std::string(reinterpret_cast<const char*>(p + pos + 8), length - 8));
        out.back() = Text(reinterpret_cast<const uint8_t*>(out.back().data()), out.back().size());
        pos += length;
    }
    return out;
}

inline const char* ListLabel(uint32_t type) {
    switch (type) {
    case kUnitWeapons: return "weapon";
    case kUnitArmors: return "armor";
    case kUnitSpells: return "spell";
    case kUnitQuickItems: return "quick item";
    case kUnitQuestItems: return "quest item";
    }
    return "";
}

// Calls f(type, payload, payloadSize, start, end) for each node in [start, end).
template <typename F> inline void EachNode(const uint8_t* d, size_t start, size_t end, F f) {
    for (size_t pos = start; pos + 8 <= end;) {
        uint32_t t = U32(d + pos), l = U32(d + pos + 4);
        if (l < 8 || pos + l > end) return;
        f(t, d + pos + 8, static_cast<size_t>(l - 8), pos + 8, pos + l);
        pos += l;
    }
}

inline Vec3 Plot(const uint8_t* p) { return {F32(p), F32(p + 4), F32(p + 8)}; }

inline Logic ReadLogic(const uint8_t* d, size_t start, size_t end) {
    Logic g;
    EachNode(d, start, end, [&](uint32_t t, const uint8_t* p, size_t n, size_t s, size_t e) {
        switch (t) {
        case kLogicCyclic: if (n >= 1) g.cyclic = p[0] != 0; break;
        case kLogicModel: if (n == 4) g.model = static_cast<int>(U32(p)); break;
        case kLogicGuardRadius: if (n == 4) g.guardRadius = F32(p); break;
        case kLogicGuardPlace: if (n == 12) g.guardPlace = Plot(p); break;
        case kLogicAlarmCount: if (n >= 1) g.alarmCount = p[0]; break;
        case kLogicUse: if (n >= 1) g.use = p[0] != 0; break;
        case kLogicWait: if (n == 4) g.wait = F32(p); break;
        case kLogicAlarmCondition: if (n >= 1) g.alarmCondition = p[0]; break;
        case kLogicHelp: if (n == 4) g.help = F32(p); break;
        case kLogicAlwaysActive: if (n >= 1) g.alwaysActive = p[0]; break;
        case kLogicAggression: if (n >= 1) g.aggression = p[0]; break;
        case kGuardPoint: {
            PatrolPoint pt;
            EachNode(d, s, e, [&](uint32_t gt, const uint8_t* gp, size_t gn, size_t gs, size_t ge) {
                if (gt == kGuardPointPosition && gn == 12) pt.position = Plot(gp);
                if (gt != kActionPoint) return;
                LookPoint look;
                EachNode(d, gs, ge, [&](uint32_t at, const uint8_t* ap, size_t an, size_t, size_t) {
                    if (at == kActionLookPoint && an == 12) look.position = Plot(ap);
                    else if (at == kActionWait && an == 4) look.wait = U32(ap);
                    else if (at == kActionTurnSpeed && an == 4) look.turnSpeed = F32(ap);
                    else if (at == kActionFlags && an >= 1) look.flags = ap[0];
                });
                pt.looks.push_back(look);
            });
            g.patrol.push_back(std::move(pt));
            break;
        }
        default: break;
        }
    });
    return g;
}

inline bool ReadObject(const uint8_t* d, size_t start, size_t end, uint32_t nodeType, Object& o, File& file) {
    switch (nodeType) {
    case kObject: o.kind = Kind::Object; break;
    case kUnit: o.kind = Kind::Unit; break;
    case kLever: o.kind = Kind::Lever; break;
    case kTorch: o.kind = Kind::Torch; break;
    case kMagicTrap: o.kind = Kind::MagicTrap; break;
    case kLight: o.kind = Kind::Light; break;
    case kParticle: o.kind = Kind::Particle; break;
    case kSound: o.kind = Kind::Sound; break;
    default: return false;
    }
    for (size_t pos = start + 8; pos + 8 <= end;) {
        uint32_t t = U32(d + pos), l = U32(d + pos + 4);
        if (l < 8 || pos + l > end) {
            file.structure.push_back({'E', "a field of the " + std::string(KindName(o.kind)) + " at byte " + std::to_string(start) +
                                               " declares a length that runs past the object (the file is damaged)"});
            break;
        }
        const uint8_t* p = d + pos + 8;
        const size_t n = l - 8;
        switch (t) {
        case kNid: case kLightId: case kParticleId: case kSoundId:
            if (n == 4) { o.id = U32(p); o.hasId = true; o.idAt = pos + 8; }
            break;
        case kObjName: case kLightName: o.name = Text(p, n); break;
        case kParticleName: o.particleName = Text(p, n); break;
        case kSoundName: o.name = Text(p, n); break;
        case kObjTemplate: o.templ = Text(p, n); break;
        case kObjPrimTexture: o.primTexture = Text(p, n); break;
        case kObjSecTexture: o.secTexture = Text(p, n); break;
        case kParentTemplate: o.parentTemplate = Text(p, n); break;
        case kObjComments: o.comments = Text(p, n); break;
        case kObjQuestInfo: o.questInfo = Text(p, n); break;
        case kUnitPrototype: o.prototype = Text(p, n); break;
        case kUnitNeedImport: if (n == 1) o.needImport = p[0] != 0; break;
        case kUnitStats: o.stats.assign(p, p + n); break;
        case kMagicTrapSpell: o.spell = Text(p, n); break;
        case kMagicTrapAreas: case kMagicTrapTargets: {
            const bool areas = t == kMagicTrapAreas;
            const size_t per = areas ? 12 : 8;
            if (n < 4 || n != 4 + static_cast<size_t>(U32(p)) * per) break;
            for (uint32_t i = 0; i < U32(p); ++i) {
                const uint8_t* e = p + 4 + i * per;
                (areas ? o.trapAreas : o.trapTargets).push_back({F32(e), F32(e + 4), areas ? F32(e + 8) : 0.0f});
            }
            break;
        }
        case kObjPosition: case kLightPosition: case kParticlePosition: case kSoundPosition:
            if (n == 12) { o.position = {F32(p), F32(p + 4), F32(p + 8)}; o.positionAt = pos + 8; }
            break;
        case kObjRotation:
            if (n == 16) { for (int i = 0; i < 4; ++i) o.rotation[i] = F32(p + 4 * i); o.rotationAt = pos + 8; }
            break;
        case kObjComplection: if (n == 12) { o.complection = {F32(p), F32(p + 4), F32(p + 8)}; o.complectionAt = pos + 8; } break;
        case kLightColor: if (n == 12) o.color = {F32(p), F32(p + 4), F32(p + 8)}; break;
        case kLightRange: if (n == 4) o.range = F32(p); break;
        case kSoundRange: if (n == 4) o.range = static_cast<float>(U32(p)); break;
        case kParticleScale: if (n == 4) o.scale = F32(p); break;
        case kParticleType: if (n == 4) o.type = static_cast<int>(U32(p)); break;
        case kObjType: if (n == 4) o.type = static_cast<int>(U32(p)); break;
        case kObjPlayer: if (n == 1) o.player = p[0]; break;
        case kObjParentId: if (n == 4) { o.parentId = U32(p); o.hasParent = true; } break;
        case kObjBodyParts: o.bodyParts = StringArray(p, n); break;
        case kSoundResName: o.soundFiles = StringArray(p, n); break;
        case kUnitWeapons: case kUnitArmors: case kUnitSpells: case kUnitQuickItems: case kUnitQuestItems: {
            ItemList list;
            list.type = t;
            list.label = ListLabel(t);
            list.entries = StringArray(p, n, &list.truncated, &list.badLength, &list.badLengthValue, &list.declaredCount);
            o.lists.push_back(std::move(list));
            break;
        }
        case kUnitLogic: o.logics.push_back(ReadLogic(d, pos + 8, pos + l)); break;
        default: break;
        }
        pos += l;
    }
    return true;
}

inline void ReadRanges(const uint8_t* d, size_t start, size_t end, std::vector<Range>& out) {
    for (size_t pos = start; pos + 8 <= end;) {
        uint32_t t = U32(d + pos), l = U32(d + pos + 4);
        if (l < 8 || pos + l > end) return;
        if (t == 65282) { // RANGE: MIN_ID, MAX_ID
            Range r;
            for (size_t q = pos + 8; q + 8 <= pos + l;) {
                uint32_t ft = U32(d + q), fl = U32(d + q + 4);
                if (fl < 8 || q + fl > pos + l) break;
                if (ft == kMinId && fl == 12) { r.min = U32(d + q + 8); r.minAt = q + 8; }
                if (ft == kMaxId && fl == 12) { r.max = U32(d + q + 8); r.maxAt = q + 8; }
                q += fl;
            }
            out.push_back(r);
        }
        pos += l;
    }
}

// The map's kind, from the node after the header: SC_OBJECT_DB_FILE (0xC000) for a zone's own map, which
// carries WORLD_SET; PR_OBJECT_DB_FILE (0xD000) for a quest's map, loaded over a zone.
enum : uint32_t { kBaseMob = 49152, kQuestMob = 53248 };
inline bool IsQuestMob(const File& f) { return f.bytes.size() >= 16 && U32(f.bytes.data() + 8) == kQuestMob; }
inline bool SetQuestMob(File& f, bool quest) {
    if (f.bytes.size() < 16 || (U32(f.bytes.data() + 8) != kBaseMob && U32(f.bytes.data() + 8) != kQuestMob)) return false;
    const uint32_t t = quest ? kQuestMob : kBaseMob;
    std::memcpy(f.bytes.data() + 8, &t, 4);
    return true;
}

inline bool Parse(File& f) {
    const std::vector<uint8_t>& b = f.bytes;
    const uint8_t* d = b.data();
    if (b.size() < 16) { f.error = "only " + std::to_string(b.size()) + " bytes: too small for a .mob"; return false; }
    if (U32(d) != kObjectDbFile) {
        f.error = "not a .mob file (its first node is " + std::to_string(U32(d)) + ", not OBJECTDBFILE)";
        return false;
    }
    bool objectSection = false;
    for (size_t pos = 16; pos + 8 <= b.size();) {
        uint32_t t = U32(d + pos), l = U32(d + pos + 4);
        if (t == kRoot) break;
        if (l < 8 || pos + l > b.size()) {
            f.structure.push_back({'E', "a node at byte " + std::to_string(pos) + " declares " + std::to_string(l) +
                                            " bytes, past the end of the file: the file is truncated or damaged"});
            break;
        }
        if (t == kScript) { f.script = DecryptScript(d + pos + 8, l - 8); f.hasScript = true; }
        else if (t == kScriptOld) { f.script = Text(d + pos + 8, l - 8); f.hasScript = true; }
        else if (t == kMainRange) ReadRanges(d, pos + 8, pos + l, f.mainRanges);
        else if (t == kSecRange) ReadRanges(d, pos + 8, pos + l, f.secRanges);
        else if (t == kAiGraph) {
            f.aiGraphBytes = l;
            // [W][H], then per layer (8) and node row: the rows' costs (W x 8 u16), representative tiles (W
            // bytes) and components (W u16). Taken only when the size matches that layout.
            if (l >= 16) {
                const uint32_t w = U32(d + pos + 8), h = U32(d + pos + 12);
                if (w && h && w <= 4096 && h <= 4096 && 8 + 8 + 8ull * w * h * 19 == l) { f.aiGraphAt = pos + 8; f.aiW = static_cast<int>(w); f.aiH = static_cast<int>(h); }
            }
        }
        else if (t == 3722304977u) { // DIPLOMATION: DIPLOMATION_FOF (1024 ints), DIPLOMATION_PL_NAMES
            EachNode(d, pos + 8, pos + l, [&](uint32_t dt, const uint8_t* p, size_t n, size_t s, size_t) {
                if (dt == 3722304978u && n == 4096) {
                    f.hasDiplomacy = true;
                    f.diplomacyAt = s;
                    f.diplomacy.resize(1024);
                    for (int i = 0; i < 1024; ++i) f.diplomacy[i] = static_cast<int32_t>(U32(p + 4 * i));
                } else if (dt == 3722304979u) {
                    f.diplomacyNames = StringArray(p, n);
                }
            });
        }
        else if (t == kWorldSet) {
            f.hasWorld = true;
            EachNode(d, pos + 8, pos + l, [&](uint32_t wt, const uint8_t* p, size_t n, size_t, size_t) {
                if (wt == kWorldWindDir && n == 12) { f.worldWindDir = Plot(p); return; }
                if (n != 4) return;
                if (wt == kWorldWindStr) f.worldWindStr = F32(p);
                else if (wt == kWorldTime) f.worldTime = F32(p);
                else if (wt == kWorldAmbient) f.worldAmbient = F32(p);
                else if (wt == kWorldSunLight) f.worldSunLight = F32(p);
            });
        }
        else if (t == kObjectSection) {
            objectSection = true;
            f.objectSectionAt = pos;
            const size_t end = pos + l;
            for (size_t c = pos + 8; c + 8 <= end;) {
                uint32_t ct = U32(d + c), cl = U32(d + c + 4);
                if (cl < 8 || c + cl > end) {
                    f.structure.push_back({'E', "a corrupted object entry at byte " + std::to_string(c) + " inside OBJECT_SECTION"});
                    break;
                }
                Object o;
                o.offset = c;
                if (ReadObject(d, c, c + cl, ct, o, f)) f.objects.push_back(std::move(o));
                c += cl;
            }
        }
        pos += l;
    }
    if (!objectSection) f.structure.push_back({'I', "no OBJECT_SECTION (a placement-only or menu map?)"});
    return true;
}

// ---- editing: fixed-size fields patched in place ------------------------------------------------

inline void PutF32(std::vector<uint8_t>& b, size_t at, float v) { std::memcpy(b.data() + at, &v, 4); }

inline void SetPosition(File& f, Object& o, const Vec3& p) {
    o.position = p;
    if (!o.positionAt) return;
    PutF32(f.bytes, o.positionAt, p.x); PutF32(f.bytes, o.positionAt + 4, p.y); PutF32(f.bytes, o.positionAt + 8, p.z);
}

inline void SetRotation(File& f, Object& o, const float q[4]) { // w, x, y, z
    for (int i = 0; i < 4; ++i) o.rotation[i] = q[i];
    if (!o.rotationAt) return;
    for (int i = 0; i < 4; ++i) PutF32(f.bytes, o.rotationAt + 4 * i, q[i]);
}

inline void SetComplection(File& f, Object& o, const Vec3& c) {
    o.complection = c;
    if (!o.complectionAt) return;
    PutF32(f.bytes, o.complectionAt, c.x); PutF32(f.bytes, o.complectionAt + 4, c.y); PutF32(f.bytes, o.complectionAt + 8, c.z);
}

inline void SetDiplomacy(File& f, int row, int col, int32_t v) {
    if (!f.hasDiplomacy || row < 0 || col < 0 || row >= 32 || col >= 32) return;
    f.diplomacy[static_cast<size_t>(row) * 32 + col] = v;
    std::memcpy(f.bytes.data() + f.diplomacyAt + (static_cast<size_t>(row) * 32 + col) * 4, &v, 4);
}

inline void PutU32(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }

inline void SetId(File& f, Object& o, uint32_t id) {
    o.id = id;
    if (o.idAt) PutU32(f.bytes, o.idAt, id);
}

inline void SetRange(File& f, Range& r, uint32_t min, uint32_t max) {
    r.min = min;
    r.max = max;
    if (r.minAt) PutU32(f.bytes, r.minAt, min);
    if (r.maxAt) PutU32(f.bytes, r.maxAt, max);
}

// Reads the objects, ranges and script again from f.bytes (after a change of sizes moved things).
inline void Reparse(File& f) {
    File g;
    g.path = f.path;
    g.fileName = f.fileName;
    g.bytes = std::move(f.bytes);
    g.loaded = Parse(g);
    f = std::move(g);
}

// The node of one of an object's fields (its type), as an offset in the file; 0 when the object has none.
inline size_t FindField(const File& f, int objectIndex, uint32_t type) {
    if (objectIndex < 0 || objectIndex >= static_cast<int>(f.objects.size())) return 0;
    const size_t start = f.objects[objectIndex].offset;
    const size_t end = start + U32(f.bytes.data() + start + 4);
    for (size_t pos = start + 8; pos + 8 <= end;) {
        uint32_t t = U32(f.bytes.data() + pos), l = U32(f.bytes.data() + pos + 4);
        if (l < 8 || pos + l > end) return 0;
        if (t == type) return pos;
        pos += l;
    }
    return 0;
}

// Replaces the payload of one of an object's fields, whatever its new size: the lengths of the nodes
// holding it (the object, OBJECTSECTION, and the root when its declared length covers it) follow, and
// the file is read again. Everything else keeps its bytes. False when the object has no such field.
inline bool ReplaceField(File& f, int objectIndex, uint32_t type, const std::vector<uint8_t>& payload) {
    const size_t at = FindField(f, objectIndex, type);
    if (!at) return false;
    const uint32_t oldLength = U32(f.bytes.data() + at + 4);
    const uint32_t newLength = static_cast<uint32_t>(8 + payload.size());
    const int64_t delta = static_cast<int64_t>(newLength) - oldLength;
    std::vector<uint8_t> b;
    b.reserve(f.bytes.size() + (delta > 0 ? delta : 0));
    b.insert(b.end(), f.bytes.begin(), f.bytes.begin() + at);
    const uint8_t* th = reinterpret_cast<const uint8_t*>(&type);
    const uint8_t* lh = reinterpret_cast<const uint8_t*>(&newLength);
    b.insert(b.end(), th, th + 4);
    b.insert(b.end(), lh, lh + 4);
    b.insert(b.end(), payload.begin(), payload.end());
    b.insert(b.end(), f.bytes.begin() + at + oldLength, f.bytes.end());
    if (delta) {
        auto grow = [&](size_t node) { PutU32(b, node + 4, static_cast<uint32_t>(U32(b.data() + node + 4) + delta)); };
        grow(f.objects[objectIndex].offset);
        if (f.objectSectionAt) grow(f.objectSectionAt);
        if (at < U32(f.bytes.data() + 4)) grow(0); // the root's length, when it still covers the objects
    }
    f.bytes = std::move(b);
    Reparse(f);
    return true;
}

inline std::vector<uint8_t> TextPayload(const std::string& cp1251) { return std::vector<uint8_t>(cp1251.begin(), cp1251.end()); }

inline std::vector<uint8_t> U32Payload(uint32_t v) {
    std::vector<uint8_t> p(4);
    std::memcpy(p.data(), &v, 4);
    return p;
}

// A unit's item list as the files store it: [count] then per entry [its list's type][8 + length][text].
inline std::vector<uint8_t> StringArrayPayload(uint32_t entryType, const std::vector<std::string>& entries) {
    std::vector<uint8_t> p = U32Payload(static_cast<uint32_t>(entries.size()));
    for (const std::string& e : entries) {
        std::vector<uint8_t> t = U32Payload(entryType), l = U32Payload(static_cast<uint32_t>(8 + e.size()));
        p.insert(p.end(), t.begin(), t.end());
        p.insert(p.end(), l.begin(), l.end());
        p.insert(p.end(), e.begin(), e.end());
    }
    return p;
}

// ---- whole objects: remove, insert, and edit a copy's bytes ------------------------------------------

// Removes these objects (indices) from the file; the object section and the root (when its length
// covers it) shrink with them. The file is read again.
inline void RemoveObjects(File& f, std::vector<int> indices) {
    if (!f.objectSectionAt || indices.empty()) return;
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    const uint32_t rootLength = U32(f.bytes.data() + 4);
    std::vector<uint8_t> b;
    b.reserve(f.bytes.size());
    size_t from = 0;
    uint32_t removed = 0, removedInRoot = 0;
    for (int oi : indices) {
        if (oi < 0 || oi >= static_cast<int>(f.objects.size())) continue;
        const size_t at = f.objects[oi].offset;
        const uint32_t len = U32(f.bytes.data() + at + 4);
        b.insert(b.end(), f.bytes.begin() + from, f.bytes.begin() + at);
        from = at + len;
        removed += len;
        if (at < rootLength) removedInRoot += len;
    }
    b.insert(b.end(), f.bytes.begin() + from, f.bytes.end());
    PutU32(b, f.objectSectionAt + 4, U32(b.data() + f.objectSectionAt + 4) - removed);
    PutU32(b, 4, rootLength - removedInRoot);
    f.bytes = std::move(b);
    Reparse(f);
}

// Appends whole object nodes (as copied from a file) at the end of the object section. Returns the
// indices the new objects get (the last ones).
inline std::vector<int> InsertObjects(File& f, const std::vector<std::vector<uint8_t>>& nodes) {
    std::vector<int> added;
    if (!f.objectSectionAt || nodes.empty()) return added;
    const size_t sectionEnd = f.objectSectionAt + U32(f.bytes.data() + f.objectSectionAt + 4);
    const uint32_t rootLength = U32(f.bytes.data() + 4);
    size_t total = 0;
    for (const auto& n : nodes) total += n.size();
    std::vector<uint8_t> b;
    b.reserve(f.bytes.size() + total);
    b.insert(b.end(), f.bytes.begin(), f.bytes.begin() + sectionEnd);
    for (const auto& n : nodes) b.insert(b.end(), n.begin(), n.end());
    b.insert(b.end(), f.bytes.begin() + sectionEnd, f.bytes.end());
    PutU32(b, f.objectSectionAt + 4, static_cast<uint32_t>(U32(b.data() + f.objectSectionAt + 4) + total));
    if (f.objectSectionAt < rootLength && sectionEnd <= rootLength) PutU32(b, 4, static_cast<uint32_t>(rootLength + total));
    const size_t before = f.objects.size();
    f.bytes = std::move(b);
    Reparse(f);
    for (size_t i = before; i < f.objects.size(); ++i) added.push_back(static_cast<int>(i));
    return added;
}

// A copy of an object's node, to change and insert elsewhere.
inline std::vector<uint8_t> ObjectNode(const File& f, int objectIndex) {
    const size_t at = f.objects[objectIndex].offset;
    return std::vector<uint8_t>(f.bytes.begin() + at, f.bytes.begin() + at + U32(f.bytes.data() + at + 4));
}

// Calls fn(position-in-node, type, length) for every node inside a copied object node, going into the
// records a unit's logic is made of (UNIT_LOGIC, GUARD_PT, ACTION_PT).
template <typename F> inline void EachInNode(const std::vector<uint8_t>& node, size_t start, size_t end, F fn) {
    for (size_t pos = start; pos + 8 <= end;) {
        uint32_t t = U32(node.data() + pos), l = U32(node.data() + pos + 4);
        if (l < 8 || pos + l > end) return;
        fn(pos, t, l);
        if (t == kUnitLogic || t == kGuardPoint || t == kActionPoint) EachInNode(node, pos + 8, pos + l, fn);
        pos += l;
    }
}

inline void NodeSetId(std::vector<uint8_t>& node, uint32_t id) {
    EachInNode(node, 8, node.size(), [&](size_t pos, uint32_t t, uint32_t l) {
        if ((t == kNid || t == kLightId || t == kParticleId || t == kSoundId) && l == 12) PutU32(node, pos + 8, id);
    });
}

// Moves a copied object by (dx, dy, dz), its unit logic's places with it (patrol, look and guard points).
inline void NodeMove(std::vector<uint8_t>& node, float dx, float dy, float dz) {
    EachInNode(node, 8, node.size(), [&](size_t pos, uint32_t t, uint32_t l) {
        const bool own = t == kObjPosition || t == kLightPosition || t == kParticlePosition || t == kSoundPosition;
        const bool logic = t == kGuardPointPosition || t == kActionLookPoint || t == kLogicGuardPlace;
        if ((own || logic) && l == 20) {
            PutF32(node, pos + 8, F32(node.data() + pos + 8) + dx);
            PutF32(node, pos + 12, F32(node.data() + pos + 12) + dy);
            if (own) PutF32(node, pos + 16, F32(node.data() + pos + 16) + dz);
        }
    });
}

// Replaces one direct field of a copied object node (any new size); false when it has none.
inline bool NodeReplaceField(std::vector<uint8_t>& node, uint32_t type, const std::vector<uint8_t>& payload) {
    for (size_t pos = 8; pos + 8 <= node.size();) {
        uint32_t t = U32(node.data() + pos), l = U32(node.data() + pos + 4);
        if (l < 8 || pos + l > node.size()) return false;
        if (t == type) {
            std::vector<uint8_t> n(node.begin(), node.begin() + pos);
            uint32_t newLength = static_cast<uint32_t>(8 + payload.size());
            const uint8_t* th = reinterpret_cast<const uint8_t*>(&type);
            const uint8_t* lh = reinterpret_cast<const uint8_t*>(&newLength);
            n.insert(n.end(), th, th + 4);
            n.insert(n.end(), lh, lh + 4);
            n.insert(n.end(), payload.begin(), payload.end());
            n.insert(n.end(), node.begin() + pos + l, node.end());
            PutU32(n, 4, static_cast<uint32_t>(n.size()));
            node = std::move(n);
            return true;
        }
        pos += l;
    }
    return false;
}

// Replaces the mission script (cp1251 text): encrypted with the file's own key when it was (SS_TEXT),
// plain in the old node. Everything else stays as it was.
inline bool SetScript(File& f, const std::string& text) {
    const uint8_t* d = f.bytes.data();
    for (size_t pos = 16; pos + 8 <= f.bytes.size();) {
        const uint32_t t = U32(d + pos), l = U32(d + pos + 4);
        if (t == kRoot || l < 8 || pos + l > f.bytes.size()) break;
        if (t != kScript && t != kScriptOld) { pos += l; continue; }
        std::vector<uint8_t> payload;
        if (t == kScript) {
            uint32_t key = l >= 12 ? U32(d + pos + 8) : 0x7da7u;
            payload.resize(4 + text.size());
            std::memcpy(payload.data(), &key, 4);
            for (size_t i = 0; i < text.size(); ++i) {
                key = key * 214013u + 2531011u;
                payload[4 + i] = static_cast<uint8_t>(static_cast<uint8_t>(text[i]) ^ ((key >> 16) & 0xFF));
            }
        } else {
            payload.assign(text.begin(), text.end());
        }
        const int64_t delta = static_cast<int64_t>(payload.size() + 8) - l;
        std::vector<uint8_t> b;
        b.reserve(f.bytes.size() + (delta > 0 ? delta : 0));
        b.insert(b.end(), f.bytes.begin(), f.bytes.begin() + pos);
        const size_t at = b.size();
        b.resize(at + 8);
        PutU32(b, at, t);
        PutU32(b, at + 4, static_cast<uint32_t>(payload.size() + 8));
        b.insert(b.end(), payload.begin(), payload.end());
        b.insert(b.end(), f.bytes.begin() + pos + l, f.bytes.end());
        if (delta && pos < U32(f.bytes.data() + 4)) PutU32(b, 4, static_cast<uint32_t>(U32(b.data() + 4) + delta)); // the root's length
        f.bytes = std::move(b);
        Reparse(f);
        return true;
    }
    return false;
}

// ---- AI_GRAPH: the game's walkability graph ---------------------------------------------------------
// The game builds it (navmesh_gen.hpp does the same): a grid of W x H nodes, one per 4 x 4 world units (node
// x, y at the centre 4x + 2, 4y + 2), in 8 layers (units use the one of their AI class). Each node has the
// cost of a step in each of 8 directions, 0xFFFF where it cannot go: 0 south, 1 south-west, 2 west,
// 3 north-west, 4 north, 5 north-east, 6 east, 7 south-east. Straight steps cost about 64 (layer 0) or 127,
// diagonal ones about 90 or 180, more on harder ground.
constexpr int kAiLayers = 8;
constexpr int kAiDx[8] = {0, -1, -1, -1, 0, 1, 1, 1};
constexpr int kAiDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
inline uint16_t AiCost(const File& f, int layer, int x, int y, int dir) {
    if (!f.aiGraphAt || x < 0 || y < 0 || x >= f.aiW || y >= f.aiH || layer < 0 || layer >= kAiLayers) return 0xFFFF;
    const size_t row = static_cast<size_t>(f.aiW) * 19;
    const size_t at = f.aiGraphAt + 8 + (static_cast<size_t>(layer) * f.aiH + y) * row + static_cast<size_t>(x) * 16 + dir * 2;
    return at + 2 <= f.bytes.size() ? static_cast<uint16_t>(f.bytes[at] | (f.bytes[at + 1] << 8)) : 0xFFFF;
}
// Replaces the map's AI_GRAPH with `payload` (navgen::Generator::Payload), or adds it at the end of the
// map's top-level nodes when it has none.
inline bool SetAiGraph(File& f, const std::vector<uint8_t>& payload) {
    const uint8_t* d = f.bytes.data();
    size_t pos = 16, at = 0, len = 0;
    for (; pos + 8 <= f.bytes.size();) {
        const uint32_t t = U32(d + pos), l = U32(d + pos + 4);
        if (t == kRoot || l < 8 || pos + l > f.bytes.size()) break;
        if (t == kAiGraph) { at = pos; len = l; break; }
        pos += l;
    }
    if (!len) at = pos;
    std::vector<uint8_t> b(f.bytes.begin(), f.bytes.begin() + at);
    b.resize(at + 8);
    PutU32(b, at, kAiGraph);
    PutU32(b, at + 4, static_cast<uint32_t>(payload.size() + 8));
    b.insert(b.end(), payload.begin(), payload.end());
    b.insert(b.end(), f.bytes.begin() + at + len, f.bytes.end());
    const int64_t delta = static_cast<int64_t>(payload.size() + 8) - static_cast<int64_t>(len);
    if (delta && at < U32(f.bytes.data() + 4)) PutU32(b, 4, static_cast<uint32_t>(U32(b.data() + 4) + delta)); // the root's length
    f.bytes = std::move(b);
    Reparse(f);
    return true;
}
inline bool AiWalkable(const File& f, int layer, int x, int y) {
    for (int d = 0; d < 8; ++d) if (AiCost(f, layer, x, y, d) != 0xFFFF) return true;
    return false;
}

// A trap's areas or cast points as the file stores them.
inline std::vector<uint8_t> TrapListPayload(bool areas, const std::vector<Vec3>& v) {
    std::vector<uint8_t> p(4 + v.size() * (areas ? 12 : 8));
    const uint32_t n = static_cast<uint32_t>(v.size());
    std::memcpy(p.data(), &n, 4);
    for (size_t i = 0; i < v.size(); ++i) {
        const float f[3] = {v[i].x, v[i].y, v[i].z};
        std::memcpy(p.data() + 4 + i * (areas ? 12 : 8), f, areas ? 12 : 8);
    }
    return p;
}

// The payload offset of a WORLD_SET value (WS_*); 0 when the map has none.
inline size_t WorldSetField(const File& f, uint32_t type) {
    const uint8_t* d = f.bytes.data();
    for (size_t pos = 16; pos + 8 <= f.bytes.size();) {
        const uint32_t t = U32(d + pos), l = U32(d + pos + 4);
        if (t == kRoot || l < 8 || pos + l > f.bytes.size()) break;
        if (t == kWorldSet)
            for (size_t c = pos + 8; c + 8 <= pos + l;) {
                const uint32_t ct = U32(d + c), cl = U32(d + c + 4);
                if (cl < 8) break;
                if (ct == type) return c + 8;
                c += cl;
            }
        pos += l;
    }
    return 0;
}

// Gives a map without one a WORLD_SET (ei_maper's defaults: no wind, time 0, ambient and sun 0), before
// its object section. False when it has one already or the file is not as expected.
inline bool AddWorldSet(File& f) {
    if (f.hasWorld || !f.objectSectionAt) return false;
    std::vector<uint8_t> node(8);
    auto child = [&](uint32_t type, std::initializer_list<float> values) {
        const size_t at = node.size();
        node.resize(at + 8 + values.size() * 4);
        PutU32(node, at, type);
        PutU32(node, at + 4, static_cast<uint32_t>(8 + values.size() * 4));
        size_t k = at + 8;
        for (float v : values) { std::memcpy(node.data() + k, &v, 4); k += 4; }
    };
    child(kWorldWindDir, {0, 0, 0});
    child(kWorldWindStr, {0});
    child(kWorldTime, {0});
    child(kWorldAmbient, {0});
    child(kWorldSunLight, {0});
    PutU32(node, 0, kWorldSet);
    PutU32(node, 4, static_cast<uint32_t>(node.size()));
    const size_t at = f.objectSectionAt;
    std::vector<uint8_t> b(f.bytes.begin(), f.bytes.begin() + at);
    b.insert(b.end(), node.begin(), node.end());
    b.insert(b.end(), f.bytes.begin() + at, f.bytes.end());
    if (at < U32(f.bytes.data() + 4)) PutU32(b, 4, static_cast<uint32_t>(U32(b.data() + 4) + node.size()));
    f.bytes = std::move(b);
    Reparse(f);
    return true;
}

// Puts a new node in place of an object's (same place in the section), fixing the lengths around it.
inline void ReplaceObjectNode(File& f, int objectIndex, const std::vector<uint8_t>& node) {
    const size_t at = f.objects[objectIndex].offset;
    const uint32_t oldLength = U32(f.bytes.data() + at + 4);
    const int64_t delta = static_cast<int64_t>(node.size()) - oldLength;
    std::vector<uint8_t> b;
    b.reserve(f.bytes.size() + (delta > 0 ? delta : 0));
    b.insert(b.end(), f.bytes.begin(), f.bytes.begin() + at);
    b.insert(b.end(), node.begin(), node.end());
    b.insert(b.end(), f.bytes.begin() + at + oldLength, f.bytes.end());
    if (delta) {
        auto grow = [&](size_t n) { PutU32(b, n + 4, static_cast<uint32_t>(U32(b.data() + n + 4) + delta)); };
        if (f.objectSectionAt) grow(f.objectSectionAt);
        if (at < U32(f.bytes.data() + 4)) grow(0);
    }
    f.bytes = std::move(b);
    Reparse(f);
}

inline void AppendNode(std::vector<uint8_t>& out, uint32_t type, const void* payload, size_t n) {
    const size_t at = out.size();
    out.resize(at + 8 + n);
    PutU32(out, at, type);
    PutU32(out, at + 4, static_cast<uint32_t>(8 + n));
    if (n) std::memcpy(out.data() + at + 8, payload, n);
}

// A patrol point as ei_maper writes it: GUARD_PT (GUARD_PT_POSITION, then an ACTION_PT per look point:
// LOOK_PT, WAIT_SEG, TURN_SPEED, FLAGS).
inline std::vector<uint8_t> GuardPointNode(const PatrolPoint& pt) {
    std::vector<uint8_t> body;
    AppendNode(body, kGuardPointPosition, &pt.position, 12);
    for (const LookPoint& l : pt.looks) {
        std::vector<uint8_t> a;
        AppendNode(a, kActionLookPoint, &l.position, 12);
        AppendNode(a, kActionWait, &l.wait, 4);
        AppendNode(a, kActionTurnSpeed, &l.turnSpeed, 4);
        AppendNode(a, kActionFlags, &l.flags, 1);
        AppendNode(body, kActionPoint, a.data(), a.size());
    }
    std::vector<uint8_t> out;
    AppendNode(out, kGuardPoint, body.data(), body.size());
    return out;
}

// Writes one of a unit's logic records (the logicIndex-th UNIT_LOGIC): its values where the record has
// them, and its patrol points in place of the old ones. Other records inside it are kept as they are.
inline bool SetLogic(File& f, int objectIndex, int logicIndex, const Logic& g) {
    const std::vector<uint8_t> node = ObjectNode(f, objectIndex);
    std::vector<uint8_t> out(node.begin(), node.begin() + 8);
    int seen = 0;
    bool done = false;
    for (size_t pos = 8; pos + 8 <= node.size();) {
        const uint32_t t = U32(node.data() + pos), l = U32(node.data() + pos + 4);
        if (l < 8 || pos + l > node.size()) return false;
        if (t != kUnitLogic || seen++ != logicIndex) { out.insert(out.end(), node.begin() + pos, node.begin() + pos + l); pos += l; continue; }
        std::vector<uint8_t> body;
        for (size_t c = pos + 8; c + 8 <= pos + l;) {
            const uint32_t ct = U32(node.data() + c), cl = U32(node.data() + c + 4);
            if (cl < 8 || c + cl > pos + l) return false;
            const size_t n = cl - 8;
            const uint8_t u8[] = {static_cast<uint8_t>(g.cyclic), static_cast<uint8_t>(g.use), static_cast<uint8_t>(g.alarmCount),
                                  static_cast<uint8_t>(g.alarmCondition), static_cast<uint8_t>(g.alwaysActive), static_cast<uint8_t>(g.aggression)};
            const uint32_t model = static_cast<uint32_t>(g.model);
            const void* v = nullptr;
            size_t want = 0;
            switch (ct) {
            case kLogicCyclic: v = &u8[0]; want = 1; break;
            case kLogicUse: v = &u8[1]; want = 1; break;
            case kLogicAlarmCount: v = &u8[2]; want = 1; break;
            case kLogicAlarmCondition: v = &u8[3]; want = 1; break;
            case kLogicAlwaysActive: v = &u8[4]; want = 1; break;
            case kLogicAggression: v = &u8[5]; want = 1; break;
            case kLogicModel: v = &model; want = 4; break;
            case kLogicGuardRadius: v = &g.guardRadius; want = 4; break;
            case kLogicWait: v = &g.wait; want = 4; break;
            case kLogicHelp: v = &g.help; want = 4; break;
            case kLogicGuardPlace: v = &g.guardPlace; want = 12; break;
            default: break;
            }
            if (ct == kGuardPoint) { c += cl; continue; } // written again below
            // Flags some maps store as any non-zero byte: kept as they are while they mean the same.
            if ((ct == kLogicCyclic || ct == kLogicUse) && n == 1 && (node[c + 8] != 0) == (*static_cast<const uint8_t*>(v) != 0)) v = nullptr;
            if (v && n == want) AppendNode(body, ct, v, n);
            else body.insert(body.end(), node.begin() + c, node.begin() + c + cl);
            c += cl;
        }
        for (const PatrolPoint& pt : g.patrol) { std::vector<uint8_t> gp = GuardPointNode(pt); body.insert(body.end(), gp.begin(), gp.end()); }
        AppendNode(out, kUnitLogic, body.data(), body.size());
        done = true;
        pos += l;
    }
    if (!done) return false;
    PutU32(out, 4, static_cast<uint32_t>(out.size()));
    ReplaceObjectNode(f, objectIndex, out);
    return true;
}

// Writes the (patched) bytes back: to a temporary file first, then over the original.
inline bool Save(const File& f, std::string& err) {
    std::string tmp = f.path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o.write(reinterpret_cast<const char*>(f.bytes.data()), static_cast<std::streamsize>(f.bytes.size()))) {
            err = "cannot write " + tmp;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, f.path, ec);
    if (ec) { err = "cannot replace " + f.path + ": " + ec.message(); return false; }
    return true;
}

inline bool Load(const std::string& path, File& f) {
    f = File{};
    f.path = path;
    size_t slash = path.find_last_of("/\\");
    f.fileName = slash == std::string::npos ? path : path.substr(slash + 1);
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) { f.error = "cannot open " + path; return false; }
    f.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    f.loaded = Parse(f);
    return f.loaded;
}

} // namespace mob
