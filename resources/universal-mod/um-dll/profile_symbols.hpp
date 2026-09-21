// Names for the functions of a game without symbols, worked out from the loaded image alone.
// Pure C++17 (no Windows API), so it can be tested on a plain copy of the executable laid out by RVA.
//
//  - FindFunctions: where functions start (call targets and code pointers in data, preceded by the
//    padding or ret the compiler leaves between functions).
//  - Analyze: names for virtual methods ("CFigure::v3": the game's classes each have a tiny method
//    returning their own name, which identifies their vtable), for the constructors and destructors
//    that store a vtable, and hints for the rest: a string the function refers to, or (in a small
//    function) the rarest Windows API it calls directly.
//  - ExportSymbols: names of the exports of a loaded DLL, for the time spent outside the game.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace profsym {

struct Image {
    const uint8_t* data = nullptr;   // the image laid out by RVA, as the loader leaves it
    uint32_t base = 0;               // the address it is loaded at
    uint32_t size = 0;               // SizeOfImage
    uint32_t codeLo = 0, codeHi = 0; // the code sections (absolute addresses)
    struct Section { uint32_t lo, hi; bool code; bool initializedData; };
    std::vector<Section> sections;

    bool Parse() {
        sections.clear();
        codeLo = codeHi = 0;
        if (!data || size < 0x200) return false;
        uint32_t lfanew = U32Rva(0x3c);
        if (!Fits(lfanew, 0x100)) return false;
        uint16_t count = U16Rva(lfanew + 6);
        uint16_t optionalSize = U16Rva(lfanew + 20);
        uint32_t table = lfanew + 24 + optionalSize;
        if (!Fits(table, static_cast<uint64_t>(count) * 40)) return false;
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t at = table + i * 40u;
            uint32_t virtualSize = U32Rva(at + 8), virtualAddress = U32Rva(at + 12), flags = U32Rva(at + 36);
            if (virtualAddress >= size) continue;
            if (static_cast<uint64_t>(virtualAddress) + virtualSize > size) virtualSize = size - virtualAddress;
            Section section = { base + virtualAddress, base + virtualAddress + virtualSize, (flags & 0x20) != 0, (flags & 0x40) != 0 };
            sections.push_back(section);
            if (section.code) {
                if (codeLo == 0 || section.lo < codeLo) codeLo = section.lo;
                if (section.hi > codeHi) codeHi = section.hi;
            }
        }
        return codeHi > codeLo;
    }
    // Do [offset, offset + length) lie inside the image? 64-bit arithmetic: the numbers come from headers that
    // may be damaged, and a 32-bit sum can wrap around and pass a check it should fail.
    bool Fits(uint64_t offset, uint64_t length) const { return offset + length <= size; }
    bool Has(uint32_t va, uint32_t n = 1) const { return va >= base && va - base + n <= size && va - base + n >= va - base; }
    const uint8_t* At(uint32_t va) const { return data + (va - base); }
    uint32_t U32(uint32_t va) const { uint32_t v; memcpy(&v, At(va), 4); return v; }
    bool InCode(uint32_t va) const { return va >= codeLo && va < codeHi; }
    bool InData(uint32_t va) const {
        for (const Section& s : sections) if (s.initializedData && !s.code && va >= s.lo && va < s.hi) return true;
        return false;
    }
    uint32_t U32Rva(uint32_t rva) const { uint32_t v; memcpy(&v, data + rva, 4); return v; }
    uint16_t U16Rva(uint32_t rva) const { uint16_t v; memcpy(&v, data + rva, 2); return v; }
};

// What the compiler leaves in front of a function: the previous function's ret (or int3), which is
// good evidence on its own, or nop padding, which also appears inside code and so is only trusted for
// an aligned address or one that is called from several places.
inline bool LooksLikeFunctionStart(const Image& image, uint32_t address, bool trustedElsewhere) {
    if (address < image.codeLo + 4 || address >= image.codeHi) return false;
    const uint8_t* p = image.At(address);
    if (p[-1] == 0xCC || p[-1] == 0xC3 || p[-3] == 0xC2) return true;
    return p[-1] == 0x90 && ((address & 15) == 0 || trustedElsewhere);
}

inline std::vector<uint32_t> FindFunctions(const Image& image) {
    std::vector<uint32_t> functions;
    if (image.codeHi <= image.codeLo) return functions;
    const uint8_t* code = image.At(image.codeLo);
    const size_t size = image.codeHi - image.codeLo;
    std::unordered_map<uint32_t, int> callTargets;
    for (size_t i = 0; i + 5 <= size; ++i) {
        if (code[i] != 0xE8) continue;
        int32_t relative;
        memcpy(&relative, code + i + 1, 4);
        uint32_t target = image.codeLo + static_cast<uint32_t>(i) + 5 + static_cast<uint32_t>(relative);
        if (target >= image.codeLo && target < image.codeHi) ++callTargets[target];
    }
    for (const auto& entry : callTargets) {
        if (LooksLikeFunctionStart(image, entry.first, entry.second >= 2)) functions.push_back(entry.first);
    }
    // Virtual methods are only reached through the tables in the data sections.
    for (const Image::Section& section : image.sections) {
        if (section.code || !section.initializedData) continue;
        for (uint32_t at = section.lo; at + 4 <= section.hi; at += 4) {
            uint32_t value = image.U32(at);
            if (image.InCode(value) && LooksLikeFunctionStart(image, value, false)) functions.push_back(value);
        }
    }
    std::sort(functions.begin(), functions.end());
    functions.erase(std::unique(functions.begin(), functions.end()), functions.end());
    return functions;
}

// The names of the functions imported by address of their import-table slot ("HeapAlloc").
inline std::unordered_map<uint32_t, std::string> ImportSlots(const Image& image) {
    std::unordered_map<uint32_t, std::string> slots;
    uint32_t lfanew = image.U32Rva(0x3c);
    if (!image.Fits(lfanew, 0x80 + 24 + 104)) return slots;
    uint32_t importRva = image.U32Rva(lfanew + 24 + 104);
    if (importRva == 0 || !image.Fits(importRva, 20)) return slots;
    for (uint64_t descriptor = importRva; image.Fits(descriptor, 20); descriptor += 20) {
        uint32_t originalThunk = image.U32Rva(descriptor), nameRva = image.U32Rva(descriptor + 12), firstThunk = image.U32Rva(descriptor + 16);
        if (nameRva == 0) break;
        uint32_t lookup = originalThunk ? originalThunk : firstThunk;
        for (uint64_t k = 0; image.Fits(static_cast<uint64_t>(lookup) + k * 4, 4); ++k) {
            uint32_t entry = image.U32Rva(lookup + k * 4);
            if (entry == 0) break;
            if (entry & 0x80000000u) continue;                 // by ordinal
            if (!image.Fits(entry, 6)) continue;
            const char* name = reinterpret_cast<const char*>(image.data + entry + 2);
            size_t length = 0;
            while (length < 64 && image.Fits(static_cast<uint64_t>(entry) + 2 + length, 1) && name[length]) ++length;
            slots[static_cast<uint32_t>(image.base + firstThunk + k * 4)] = std::string(name, length);
        }
    }
    return slots;
}

struct Symbols {
    std::unordered_map<uint32_t, std::string> names; // function -> "CFigure::v3"
    std::unordered_map<uint32_t, std::string> hints; // function -> "\"a string\"" or "->ApiName"
    size_t vtables = 0, namedVtables = 0;
};

inline bool ReadHintString(const Image& image, uint32_t address, std::string* out) {
    if (!image.Has(address, 8) || !image.InData(address)) return false;
    const uint8_t* text = image.At(address);
    uint32_t room = image.base + image.size - address;
    size_t length = 0;
    size_t letters = 0;
    while (length < 48 && length < room && text[length] != 0) {
        if (text[length] < 0x20 || text[length] > 0x7E) return false;
        if ((text[length] | 0x20) >= 'a' && (text[length] | 0x20) <= 'z') ++letters;
        ++length;
    }
    if (length < 6 || letters < 4 || text[0] == '%') return false;
    *out = std::string(reinterpret_cast<const char*>(text), length);
    return true;
}

inline Symbols Analyze(const Image& image, const std::vector<uint32_t>& functions) {
    Symbols result;
    if (image.codeHi <= image.codeLo) return result;
    const uint8_t* code = image.At(image.codeLo);
    const size_t codeSize = image.codeHi - image.codeLo;
    auto functionOf = [&](uint32_t address) -> uint32_t {
        auto after = std::upper_bound(functions.begin(), functions.end(), address);
        if (after == functions.begin()) return 0;
        return address - *(after - 1) < 0x10000 ? *(after - 1) : 0;
    };

    // 1. tiny methods that return the class's own name: mov eax, offset "CFigure" / ret
    std::unordered_map<uint32_t, std::string> classGetters;
    for (size_t i = 0; i + 6 <= codeSize; ++i) {
        if (code[i] != 0xB8 || code[i + 5] != 0xC3) continue;
        uint32_t text;
        memcpy(&text, code + i + 1, 4);
        if (!image.Has(text, 4) || !image.InData(text)) continue;
        const char* name = reinterpret_cast<const char*>(image.At(text));
        size_t length = 0;
        while (length < 44 && text - image.base + length < image.size && name[length]) ++length;
        if (length < 3 || length > 40 || name[0] != 'C' || name[1] < 'A' || name[1] > 'Z') continue;
        bool identifier = true;
        for (size_t k = 0; k < length; ++k) {
            char c = name[k];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) identifier = false;
        }
        if (identifier) classGetters[image.codeLo + static_cast<uint32_t>(i)] = std::string(name, length);
    }

    // 2. vtables: what a constructor stores into the object, mov [reg], offset table
    std::unordered_map<uint32_t, std::vector<uint32_t>> stores; // vtable -> the addresses that store it
    for (size_t i = 0; i + 6 <= codeSize; ++i) {
        if (code[i] != 0xC7 || code[i + 1] > 0x07) continue;
        uint32_t table;
        memcpy(&table, code + i + 2, 4);
        if (!image.Has(table, 8) || image.InCode(table) || !image.InData(table)) continue;
        if (!image.InCode(image.U32(table)) || !image.InCode(image.U32(table + 4))) continue;
        stores[table].push_back(image.codeLo + static_cast<uint32_t>(i));
    }
    std::vector<uint32_t> tables;
    for (const auto& entry : stores) tables.push_back(entry.first);
    std::sort(tables.begin(), tables.end());
    result.vtables = tables.size();

    struct Slot { std::string cls; uint32_t index; uint32_t slots; };
    std::unordered_map<uint32_t, Slot> owner;      // function -> the smallest named vtable holding it
    std::unordered_map<uint32_t, std::string> tableClass;
    for (size_t t = 0; t < tables.size(); ++t) {
        uint32_t table = tables[t];
        uint32_t limit = t + 1 < tables.size() ? tables[t + 1] : image.base + image.size;
        uint32_t slots = 0;
        while (table + slots * 4 + 4 <= limit && image.Has(table + slots * 4, 4) && image.InCode(image.U32(table + slots * 4))) ++slots;
        std::string cls;
        for (uint32_t k = 0; k < slots && cls.empty(); ++k) {
            auto getter = classGetters.find(image.U32(table + k * 4));
            if (getter != classGetters.end()) cls = getter->second;
        }
        if (cls.empty()) continue;
        tableClass[table] = cls;
        ++result.namedVtables;
        for (uint32_t k = 0; k < slots; ++k) {
            uint32_t function = image.U32(table + k * 4);
            auto known = owner.find(function);
            if (known == owner.end() || slots < known->second.slots) owner[function] = Slot{cls, k, slots};
        }
    }
    for (const auto& entry : owner) {
        char text[80];
        snprintf(text, sizeof(text), "%s::v%u", entry.second.cls.c_str(), entry.second.index);
        result.names[entry.first] = text;
    }
    // constructors and destructors: the function that stores a named vtable
    for (const auto& entry : stores) {
        auto cls = tableClass.find(entry.first);
        if (cls == tableClass.end()) continue;
        for (uint32_t at : entry.second) {
            uint32_t function = functionOf(at);
            if (function && !result.names.count(function)) result.names[function] = cls->second + "::ctor/dtor";
        }
    }

    // 3. hints for the rest: a string the function refers to, else the rarest API it calls
    std::unordered_map<uint32_t, std::string> imports = ImportSlots(image);
    std::unordered_map<std::string, int> importCalls;
    for (size_t i = 0; i + 6 <= codeSize; ++i) {
        if (code[i] != 0xFF || code[i + 1] != 0x15) continue;
        uint32_t slot;
        memcpy(&slot, code + i + 2, 4);
        auto found = imports.find(slot);
        if (found != imports.end()) ++importCalls[found->second];
    }
    for (size_t f = 0; f < functions.size(); ++f) {
        const uint32_t start = functions[f];
        if (result.names.count(start) || !image.InCode(start)) continue;
        uint32_t end = f + 1 < functions.size() ? functions[f + 1] : image.codeHi;
        if (end > start + 0x2000) end = start + 0x2000;
        if (end > image.codeHi) end = image.codeHi;
        std::string text;
        std::string rarestApi;
        int rarest = 1 << 30;
        for (uint32_t at = start; at + 6 <= end; ++at) {
            const uint8_t* p = image.At(at);
            if ((p[0] == 0x68 || (p[0] >= 0xB8 && p[0] <= 0xBF)) && text.empty()) {
                uint32_t value;
                memcpy(&value, p + 1, 4);
                if (value >= image.base && ReadHintString(image, value, &text)) continue;
                text.clear();
            } else if (p[0] == 0xFF && p[1] == 0x15) {
                uint32_t slot;
                memcpy(&slot, p + 2, 4);
                auto found = imports.find(slot);
                if (found != imports.end()) {
                    int calls = importCalls[found->second];
                    if (calls <= 60 && calls < rarest) { rarest = calls; rarestApi = found->second; }
                }
            }
        }
        if (!text.empty() && end - start <= 1024) {
            // A string in a big function is one of many, and usually an error message: only small ones get it.
            if (text.size() > 26) text.resize(26);
            result.hints[start] = "mentions \"" + text + "\"";
        } else if (text.empty() && !rarestApi.empty() && end - start <= 2048) {
            // In a big function the rarest API is close to arbitrary, so only small ones get this hint.
            result.hints[start] = "(calls " + rarestApi + ")";
        }
    }
    return result;
}

// The exports of a loaded DLL, sorted by address, to name a code address inside it.
struct ExportSymbols {
    std::vector<std::pair<uint32_t, std::string>> byRva;
    uint32_t size = 0;
    bool Load(const Image& module) {
        byRva.clear();
        size = module.size;
        if (!module.data || module.size < 0x200) return false;
        uint32_t lfanew = module.U32Rva(0x3c);
        if (!module.Fits(lfanew, 0x80 + 24 + 96)) return false;
        uint32_t exportRva = module.U32Rva(lfanew + 24 + 96);
        if (exportRva == 0 || !module.Fits(exportRva, 40)) return false;
        uint32_t names = module.U32Rva(exportRva + 24), functions = module.U32Rva(exportRva + 28);
        uint32_t nameTable = module.U32Rva(exportRva + 32), ordinalTable = module.U32Rva(exportRva + 36);
        if (names > 20000 || !module.Fits(nameTable, static_cast<uint64_t>(names) * 4) || !module.Fits(ordinalTable, static_cast<uint64_t>(names) * 2)) return false;
        for (uint32_t i = 0; i < names; ++i) {
            uint32_t nameRva = module.U32Rva(nameTable + i * 4);
            uint16_t ordinal = module.U16Rva(ordinalTable + i * 2);
            if (nameRva >= module.size || !module.Fits(static_cast<uint64_t>(functions) + ordinal * 4u, 4)) continue;
            uint32_t function = module.U32Rva(functions + ordinal * 4u);
            if (function == 0 || function >= module.size) continue;
            const char* name = reinterpret_cast<const char*>(module.data + nameRva);
            size_t length = 0;
            while (length < 64 && module.Fits(static_cast<uint64_t>(nameRva) + length, 1) && name[length]) ++length;
            byRva.push_back(std::make_pair(function, std::string(name, length)));
        }
        std::sort(byRva.begin(), byRva.end());
        return !byRva.empty();
    }
    // "name" or "name+0x1c" for the nearest export at or below the address, "" when there is none within 0x40.
    std::string Describe(uint32_t rva) const {
        auto after = std::upper_bound(byRva.begin(), byRva.end(), std::make_pair(rva, std::string("\xff")));
        if (after == byRva.begin()) return "";
        --after;
        uint32_t distance = rva - after->first;
        if (distance > 0x40) return ""; // an export is rarely longer, so this is some other, unexported function
        if (distance == 0) return after->second;
        char text[24];
        snprintf(text, sizeof(text), "+0x%X", distance);
        return after->second + text;
    }
};

} // namespace profsym
