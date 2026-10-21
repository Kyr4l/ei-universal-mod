// Host test of inireg_model.hpp: g++ -std=c++17 -I.. tests/inireg_model_test.cpp -o /tmp/inireg_model_test && /tmp/inireg_model_test
#include <cstdio>
#include "inireg_model.hpp"
using namespace inimodel;
static int fail = 0;
#define CHECK(c) do { if (!(c)) { ++fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
    const std::string text = "stray\r\n[Addon]\r\nShowAllQuests=1\r\nAntiCrash = 1\r\n\r\n[RPG]\r\nPerk Power Base=1.95\r\n; Consider Modifier=200\r\n# comment\r\n[DifficultyLevels]\r\nAbsorption=0.65\r\nAbsorption=0.33\r\n";
    auto lines = Split(text);
    CHECK(lines.size() == 12 && lines[0] == "stray" && lines[3] == "AntiCrash = 1");
    CHECK(Join(lines, true) == text && UsesCrlf(text));
    CHECK(Split("a\nb").size() == 2 && Split("a\n").size() == 1 && Split("").empty());
    CHECK(Kind("[A]") == LineKind::Section && Kind("  ") == LineKind::Blank && Kind("x=1") == LineKind::KeyValue && Kind("# no") == LineKind::Other && Kind("; Key=1") == LineKind::KeyValue && Kind("word") == LineKind::Other);
    const auto sec = Sections(lines);
    CHECK(sec.size() == 3 && sec[0].name == "Addon" && sec[1].name == "RPG" && sec[2].name == "DifficultyLevels");
    CHECK(sec[0].keys == 2 && sec[1].keys == 2 && sec[2].keys == 2);
    CHECK(sec[0].first == 2 && sec[0].last == 5 && sec[1].header == 5);
    std::string k, v; SplitKeyValue("AntiCrash = 1", k, v); CHECK(k == "AntiCrash" && v == "1");
    SplitKeyValue("a=b=c", k, v); CHECK(k == "a" && v == "b=c");
    CHECK(Classify("12") == ValueType::Int && Classify("-3") == ValueType::Int && Classify("1.95") == ValueType::Float && Classify("1e3") == ValueType::Float && Classify("abc") == ValueType::Text && Classify("") == ValueType::Text && Classify("1.2.3") == ValueType::Text);
    CHECK(CountKey(lines, sec[2], "Absorption") == 2 && CountKey(lines, sec[0], "Absorption") == 0);
    auto copy = lines; const int at = InsertKey(copy, sec[0], "New", "5");
    CHECK(at == 4 && copy[4] == "New=5" && copy[5] == "" && Sections(copy)[0].keys == 3);
    AddSection(copy, "Fresh"); CHECK(Sections(copy).back().name == "Fresh" && copy.back() == "[Fresh]");
    CHECK(Sections(Split("A=1\nB=2\n")).size() == 1 && Sections(Split("# only a comment\n")).empty());
    std::printf("%s\n", fail ? "FAILED" : "inireg model: all checks passed");
    return fail ? 1 : 0;
}
