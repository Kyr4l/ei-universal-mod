// fp_light_test.cpp: UmFpLight (fp_light.cpp) against the library's own code (3DfpFPU.dll RVA 0xD910), on random data.
// Build (needs the DLL): i686-w64-mingw32-g++ -std=c++17 -O2 -I.. fp_light_test.cpp ../fp_light.cpp -static -o fp_light_test.exe
// Run:  wine fp_light_test.exe "C:\path\3dfpfpu.dll"
#include <windows.h>
#include <float.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

extern "C" void __attribute__((thiscall)) UmFpLight(void*, const char*, const char*, uint32_t*, const uint16_t*, int, void* const*, int, const float*);
typedef void(__attribute__((thiscall)) * LightFn)(void*, const char*, const char*, uint32_t*, const uint16_t*, int, void* const*, int, const float*);

int main(int argc, char** argv) {
    HMODULE lib = LoadLibraryA(argc > 1 ? argv[1] : "3dfpfpu.dll");
    if (!lib) { std::printf("no dll\n"); return 2; }
    LightFn original = reinterpret_cast<LightFn>(reinterpret_cast<char*>(lib) + 0xD910);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1, 1), pos(-10, 10), pos01(0, 1);
    long mismatches = 0, vertices = 0, offByOne = 0;
    for (unsigned cw : {0x27Fu, 0x7Fu}) {              // 53 bit and 24 bit precision: the game's x87 state is whichever DirectDraw left
        _controlfp(cw == 0x27F ? _PC_53 : _PC_24, _MCW_PC);
        for (int round = 0; round < 3000; ++round) {
            const int blocks = 6, count = 1 + rng() % 40, lightCount = rng() % 6;
            std::vector<float> positions(blocks * 16), normals(blocks * 16);
            for (int b = 0; b < blocks; ++b) for (int lane = 0; lane < 4; ++lane) {
                float n[3] = {u(rng), u(rng), u(rng)}; float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-3f;
                for (int c = 0; c < 3; ++c) { positions[b * 16 + c * 4 + lane] = pos(rng); normals[b * 16 + c * 4 + lane] = n[c] / len; }
            }
            std::vector<uint16_t> idx(count * 2);
            for (int i = 0; i < count; ++i) { idx[2 * i] = rng() % (blocks * 4); idx[2 * i + 1] = rng() % (blocks * 4); }
            std::vector<std::vector<char>> lightMem(lightCount, std::vector<char>(0x130, 0));
            std::vector<void*> lights(lightCount);
            for (int i = 0; i < lightCount; ++i) {
                float* f = reinterpret_cast<float*>(lightMem[i].data());
                f[0x28 / 4] = pos(rng); f[0x2C / 4] = pos(rng); f[0x30 / 4] = pos(rng);
                f[0x104 / 4] = pos01(rng) * 0.05f; f[0x114 / 4] = pos01(rng); f[0x118 / 4] = pos01(rng); f[0x11C / 4] = pos01(rng);
                lights[i] = lightMem[i].data();
            }
            std::vector<char> dirMem(0x130, 0); float* d = reinterpret_cast<float*>(dirMem.data());
            d[0x108 / 4] = u(rng); d[0x10C / 4] = u(rng); d[0x110 / 4] = u(rng); d[0x114 / 4] = pos01(rng); d[0x118 / 4] = pos01(rng); d[0x11C / 4] = pos01(rng); d[0x120 / 4] = pos01(rng);
            std::vector<char> self(0x40, 0); *reinterpret_cast<char**>(self.data() + 4) = dirMem.data();
            float* fl = reinterpret_cast<float*>(self.data()); fl[0x1C / 4] = pos01(rng) * 0.3f; fl[0x20 / 4] = pos01(rng) * 0.3f; fl[0x24 / 4] = pos01(rng) * 0.3f;
            float amb[3] = {pos01(rng) * 0.4f, pos01(rng) * 0.4f, pos01(rng) * 0.4f};
            std::vector<uint32_t> a(count), b(count);
            original(self.data(), reinterpret_cast<char*>(positions.data()), reinterpret_cast<char*>(normals.data()), a.data(), idx.data(), count, lights.data(), lightCount, amb);
            UmFpLight(self.data(), reinterpret_cast<char*>(positions.data()), reinterpret_cast<char*>(normals.data()), b.data(), idx.data(), count, lights.data(), lightCount, amb);
            for (int i = 0; i < count; ++i, ++vertices) if (a[i] != b[i]) {
                ++mismatches;
                int worst = 0; for (int s = 0; s < 32; s += 8) { int x = (a[i] >> s) & 255, y = (b[i] >> s) & 255; worst = std::max(worst, std::abs(x - y)); }
                if (worst <= 1) ++offByOne;
                else if (mismatches - offByOne < 6) std::printf("  big difference: %08X vs %08X (control word %X)\\n", a[i], b[i], cw);
            }
        }
        std::printf("control word 0x%X: %ld vertices so far, %ld differ (%ld by one level of 255)\\n", cw, vertices, mismatches, offByOne);
    }
    return (mismatches == offByOne) ? 0 : 1;
}
