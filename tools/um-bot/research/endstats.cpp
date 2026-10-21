// endstats: what follows each 0x41 update that decodes (the byte after its end), for units already known and not.   g++ -std=c++17 -O1 -I. -I../um-multitool research/endstats.cpp -o /tmp/endstats ; /tmp/endstats dump/*.bin
#define private public
#include "../net.hpp"
#undef private
int main(int argc, char** argv) {
    net::Client c; std::map<int, size_t> knownNext, unknownNext; size_t knownTry = 0, unknownTry = 0, knownOk = 0, unknownOk = 0;
    c.updateHook = [&](uint32_t, bool known, size_t e, int next) { (known ? knownTry : unknownTry)++; if (!e) return; (known ? knownOk : unknownOk)++; (known ? knownNext : unknownNext)[next]++; };
    for (int a = 1; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb"); if (!f) continue;
        std::vector<uint8_t> m; uint8_t buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) m.insert(m.end(), buf, buf + n); std::fclose(f);
        c.ReadUnits(m);
    }
    std::printf("known units: %zu tried, %zu decoded; unknown ids: %zu tried, %zu decoded\n", knownTry, knownOk, unknownTry, unknownOk);
    std::printf("after a decoded update of a KNOWN unit:"); for (auto& kv : knownNext) std::printf("  %02X x%zu", kv.first & 0xFF, kv.second); std::printf("\n");
    std::printf("after a decoded update of an UNKNOWN id:"); for (auto& kv : unknownNext) std::printf("  %02X x%zu", kv.first & 0xFF, kv.second); std::printf("\n");
}
