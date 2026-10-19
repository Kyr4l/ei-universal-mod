// endcheck: how many creation records end exactly where the next sub-message starts?
#define private public
#include "../net.hpp"
#undef private
int main(int argc, char** argv) {
    net::Client c; size_t ok = 0, bad = 0, total = 0;
    for (int a = 1; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb"); if (!f) continue;
        std::vector<uint8_t> m; uint8_t buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) m.insert(m.end(), buf, buf + n); std::fclose(f);
        for (size_t o = 0; o + 20 < m.size(); ++o) {
            if (m[o] != 0x40) continue;
            const size_t e = c.Creation(m, o);
            if (!e) continue;
            ++total; const uint8_t nx = e < m.size() ? m[e] : 0x41;
            const bool good = (e == m.size() || nx == 0x40 || nx == 0x41 || nx == 0x42 || nx == 0x43 || nx == 0x60 || nx == 0x81 || nx == 0x0B || nx == 0x18 || nx == 0x0E); if (!good && std::getenv("SHOW")) { uint32_t id; std::memcpy(&id, &m[o + 1], 4); std::printf("BAD id %u flags 0x%X kind 0x%X next %02X end %zu/%zu mask?", id, c.unitInfo_[id].flags, c.unitInfo_[id].kind, nx, e, m.size()); for (int k = 0; k <= 13; ++k) if (c.unitFields_.count({id, k})) std::printf(" %d", k); std::printf("\n"); }
            if (e == m.size() || nx == 0x40 || nx == 0x41 || nx == 0x42 || nx == 0x43 || nx == 0x60 || nx == 0x81 || nx == 0x0B || nx == 0x18 || nx == 0x0E) ++ok; else ++bad;
            o = e - 1;
        }
    }
    std::printf("creations %zu, ending at a plausible next message %zu, not %zu\n", total, ok, bad);
}
