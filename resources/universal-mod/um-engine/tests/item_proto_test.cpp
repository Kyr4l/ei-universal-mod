// item_proto_test.cpp: host test of the weapon table lookups (item_proto.cpp).
// Build: g++ -std=c++17 -O2 -DUM_HOST_TEST -I.. item_proto_test.cpp ../item_proto.cpp -o item_proto_test && ./item_proto_test
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

const char* g_begin = nullptr;
const char* g_end = nullptr;
const char* g_matBegin = nullptr;
const char* g_matEnd = nullptr;
extern "C" int UmItemProtoType(unsigned itemId);
extern "C" long double UmItemPrice(unsigned itemId);
extern "C" long double UmItemPrice2(unsigned itemId);
extern "C" long double UmItemWeight(unsigned itemId);
extern "C" unsigned UmWeaponSkill(unsigned itemId);
extern "C" int UmWeaponDamageKind(const char* weapon);
extern "C" void UmEngineLog(const char* format, ...) { (void)format; }

static void PutF(std::vector<char>& v, size_t at, float f) { std::memcpy(&v[at], &f, 4); }

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } };
    std::vector<char> weapons(37 * 0xB4, 0), materials(40 * 0x50, 0);
    for (int i = 0; i < 37; ++i) {
        const size_t r = static_cast<size_t>(i) * 0xB4;
        const int kind = 100 + i; std::memcpy(&weapons[r + 0xC], &kind, 4);
        PutF(weapons, r + 0x90, 10.0f + static_cast<float>(i)); PutF(weapons, r + 0x94, 1.0f); PutF(weapons, r + 0x38, 2.0f);
        if (i == 5) PutF(weapons, r + 0x98 + 4 * 3, 1.5f);   // the skill of weapon 5 = 3
    }
    for (int i = 0; i < 40; ++i) { PutF(materials, static_cast<size_t>(i) * 0x50 + 0x28, 2.0f); PutF(materials, static_cast<size_t>(i) * 0x50 + 0x14, 0.5f); }
    g_begin = weapons.data(); g_end = weapons.data() + weapons.size();
    g_matBegin = materials.data(); g_matEnd = materials.data() + materials.size();
    check(UmItemProtoType(0) == 100, "index 0");
    check(UmItemProtoType((36u << 16) | 22) == 136, "the last record, material bits ignored");
    check(UmItemProtoType(37u << 16) == 0, "one past the end");
    check(UmItemProtoType(45u << 16) == 0, "row 45 of a 37-row table (the crash)");
    check(UmItemProtoType(0xFFFFFFFFu) == 0, "the empty-slot value");
    check(std::fabs(static_cast<double>(UmItemPrice((4u << 16) | 3)) - 28.0) < 1e-9, "price = weapon 0x90 * material 0x28");
    check(std::fabs(static_cast<double>(UmItemPrice2((4u << 16) | 3)) - 30.0) < 1e-9, "price2 = (0x94 + 0x90) * material");
    check(std::fabs(static_cast<double>(UmItemWeight((4u << 16) | 3)) - 1.0) < 1e-9, "weight = 0x38 * material 0x14");
    check(UmItemPrice(45u << 16) == 0 && UmItemPrice2(45u << 16) == 0 && UmItemWeight(45u << 16) == 0, "the three of an unknown weapon");
    check(UmItemPrice((4u << 16) | 40) == 0, "an unknown material");
    check(UmWeaponSkill(5u << 16) == 3, "skill = first non-zero float");
    check(UmWeaponSkill(4u << 16) == 6, "no skill floats: 6");
    check(UmWeaponSkill(45u << 16) == 6, "an unknown weapon: 6");
    unsigned item[8] = {};
    item[6] = (36u << 16); check(UmWeaponDamageKind(reinterpret_cast<const char*>(item)) == 136, "damage kind");
    item[6] = (45u << 16); check(UmWeaponDamageKind(reinterpret_cast<const char*>(item)) == 0, "damage kind of an unknown weapon");
    g_begin = nullptr; g_end = nullptr;
    check(UmItemProtoType(0) == 0 && UmItemPrice(0) == 0 && UmWeaponSkill(0) == 6, "an empty table (being rebuilt)");
    g_begin = weapons.data() + 100; g_end = weapons.data();
    check(UmItemProtoType(0) == 0, "end before begin");
    std::printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
