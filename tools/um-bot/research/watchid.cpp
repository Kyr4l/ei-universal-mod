// watchid: replay dump files through ReadUnits and print every change of one unit's decoded fields.  usage: watchid ID dump/*.bin
#define private public
#include "../net.hpp"
#undef private
int main(int argc, char** argv) {
    net::Client c; const uint32_t id = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 0));
    std::map<int, std::vector<uint8_t>> last;
    for (int a = 2; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb"); if (!f) continue;
        std::vector<uint8_t> m; uint8_t buf[4096]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) m.insert(m.end(), buf, buf + n); std::fclose(f);
        c.ReadUnits(m);
        for (int k = 0; k <= 13; ++k) {
            auto it = c.unitFields_.find({id, k}); if (it == c.unitFields_.end()) continue;
            if (last[k] != it->second.b) { std::printf("%s field %d:", argv[a] + std::string(argv[a]).rfind('/') + 1, k); for (size_t i = 0; i < it->second.b.size() && i < 40; ++i) std::printf("%02x", it->second.b[i]); std::printf("\n"); last[k] = it->second.b; }
        }
    }
}
