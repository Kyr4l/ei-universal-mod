// poscheck: replay UM_BOT_DUMP files and compare every raw decoded position with the true ones (um.dll UNITS sampled while the dump was taken:
// lines "time id x y"). A decoded position farther than TOL from every true sample of that unit is reported with its source field.
//   g++ -std=c++17 -O1 -I. -I../um-multitool research/poscheck.cpp -o /tmp/poscheck ; /tmp/poscheck gt.txt dump/*.bin
#define private public
#include "../net.hpp"
#undef private
int main(int argc, char** argv) {
    std::map<uint32_t, std::vector<std::pair<float, float>>> truth;
    if (FILE* f = std::fopen(argv[1], "r")) { double t; unsigned id; float x, y; while (std::fscanf(f, "%lf %u %f %f", &t, &id, &x, &y) == 4) truth[id].push_back({x, y}); std::fclose(f); }
    const float tol = std::getenv("TOL") ? static_cast<float>(std::atof(std::getenv("TOL"))) : 4.0f;
    net::Client c; size_t total = 0, bad = 0, unknown = 0; std::map<std::string, std::pair<size_t, size_t>> bySrc; const char* file = "";
    c.rawPosHook = [&](uint32_t id, float x, float y, const char* src) {
        ++total; auto it = truth.find(id); if (it == truth.end()) { ++unknown; return; }
        float best = 1e30f; for (auto& p : it->second) best = std::min(best, std::hypot(p.first - x, p.second - y));
        auto& s = bySrc[src]; ++s.first;
        if (best > tol) { ++bad; ++s.second; if (bad <= 40 || std::getenv("ALL")) std::printf("%s unit %u src %s decoded (%.1f,%.1f) nearest true %.1f\n", file, id, src, x, y, best); }
    };
    for (int a = 2; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb"); if (!f) continue;
        std::vector<uint8_t> m; uint8_t buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) m.insert(m.end(), buf, buf + n); std::fclose(f);
        file = argv[a] + std::string(argv[a]).rfind('/') + 1;
        c.ReadUnits(m);
    }
    std::printf("%zu positions (%zu of units without ground truth), %zu farther than %.1f from any true position\n", total, unknown, bad, static_cast<double>(tol));
    for (auto& kv : bySrc) std::printf("  source %s: %zu, bad %zu\n", kv.first.c_str(), kv.second.first, kv.second.second);
}
