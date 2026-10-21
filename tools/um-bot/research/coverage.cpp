// coverage: replay dump files and list the units that got 0x41 updates but no decoded creation record.
#define private public
#include "../net.hpp"
#undef private
int main(int argc, char** argv) {
    net::Client c;
    for (int a = 1; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb"); if (!f) continue;
        std::vector<uint8_t> m; uint8_t buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) m.insert(m.end(), buf, buf + n); std::fclose(f);
        c.ReadUnits(m);
    }
    std::set<uint32_t> ids; for (const auto& kv : c.unitFields_) ids.insert(kv.first.first);
    size_t miss = 0;
    for (uint32_t id : ids) if (!c.unitInfo_.count(id)) { std::printf("updated but never created: %u (0x%X) pos %s\n", id, id, c.unitPos.count(id) ? "known" : "unknown"); ++miss; }
    std::printf("%zu units with fields, %zu created, %zu without creation\n", ids.size(), c.unitInfo_.size(), miss);
}
