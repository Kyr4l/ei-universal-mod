// streamtest: feed UM_BOT_DUMP files (decompressed stream messages) through um-bot's decoder and print what it makes of them.
//   g++ -std=c++17 -O1 -I. -I../um-multitool streamtest.cpp -o /tmp/streamtest ; /tmp/streamtest dump/*.bin
#define private public
#include "../net.hpp"
#undef private
int main(int argc, char** argv) {
    net::Client c;
    size_t created = 0, bad = 0;
    for (int a = 1; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb"); if (!f) continue;
        std::vector<uint8_t> m; uint8_t buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) m.insert(m.end(), buf, buf + n); std::fclose(f);
        c.ReadUnits(m);
        for (size_t o = 0; o + 20 < m.size(); ++o) {
            if (m[o] != 0x40) continue;
            const size_t e = c.Creation(m, o);
            if (!e) continue;
            uint32_t id; std::memcpy(&id, &m[o + 1], 4);
            const auto& u = c.unitInfo_[id]; const auto pos = c.unitPos.find(id);
            std::printf("%s @%zu: id %u kind 0x%X proto %u fig '%s' tex '%s' label '%s' name '%s' flags 0x%X pos %.1f,%.1f hp %d mana %d end %zu/%zu next %02X\n", argv[a] + std::string(argv[a]).rfind('/') + 1, o, id, u.kind, u.proto, u.figure.c_str(), u.texture.c_str(), u.label.c_str(), u.name.c_str(), u.flags,
                        pos != c.unitPos.end() ? pos->second.x : -1.f, pos != c.unitPos.end() ? pos->second.y : -1.f, c.Health(id), c.Mana(id), e, m.size(), e < m.size() ? m[e] : 0);
            std::printf("    side %d enemies 0x%X\n", c.unitSide_.count(id) ? c.unitSide_[id] : -1, c.unitEnemies_.count(id) ? c.unitEnemies_[id] : 0);
            if (std::getenv("DUMPF")) for (int k = 0; k <= 13; ++k) { auto it = c.unitFields_.find({id, k}); if (it == c.unitFields_.end()) continue; std::printf("    field %d (%zu): ", k, it->second.b.size()); for (size_t i = 0; i < it->second.b.size() && i < 80; ++i) std::printf("%02x", it->second.b[i]); std::printf("\n"); }
            ++created; o = e - 1;
        }
    }
    for (const auto& kv : c.expShare_) std::printf("exp shares: hero %u total %.4f\n", kv.first, kv.second);
    std::printf("%zu creations decoded\n", created); (void)bad;
}
