// hypot_test.cpp: host test of UmHypot (crt_math.cpp) against the C library and a transcription of the game's algorithm.
// Build: g++ -std=c++17 -O2 -msse2 -I.. hypot_test.cpp ../crt_math.cpp -o hypot_test && ./hypot_test
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
extern "C" double UmHypot(double x, double y);
// The game's algorithm (0x6FD060) in long double, the stores to double where the code stores: m * sqrt(a^2 + b^2).
static double Original(double x, double y) {
    double ax = std::fabs(x), ay = std::fabs(y);
    double m = ay > ax ? ay : ax;
    if (m == 0) return 0;
    double a = static_cast<double>(static_cast<long double>(ax) / m), b = static_cast<double>(static_cast<long double>(ay) / m);
    double t = static_cast<double>(static_cast<long double>(a) * a + static_cast<long double>(b) * b);
    double s = static_cast<double>(std::sqrt(static_cast<long double>(t)));
    return static_cast<double>(static_cast<long double>(m) * s);
}
static double Ulps(double a, double b) {
    if (a == b) return 0;
    const double spacing = std::nextafter(std::fabs(b), INFINITY) - std::fabs(b);
    return std::fabs(a - b) / spacing;
}
int main() {
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> unit(-1, 1);
    double worstRef = 0, worstOrig = 0; long differ = 0, total = 0;
    for (int i = 0; i < 20000000; ++i) {
        const double scale = std::pow(10.0, std::uniform_real_distribution<double>(-6, 6)(rng));
        const double x = unit(rng) * scale, y = unit(rng) * scale;
        const double got = UmHypot(x, y);
        worstRef = std::fmax(worstRef, Ulps(got, static_cast<double>(std::hypot(static_cast<long double>(x), static_cast<long double>(y)))));
        const double o = Original(x, y);
        worstOrig = std::fmax(worstOrig, Ulps(got, o));
        differ += got != o; ++total;
    }
    // the special cases
    const double inf = INFINITY, nan = NAN;
    int bad = 0;
    bad += UmHypot(0, 0) != 0;
    bad += UmHypot(3, 4) != 5;
    bad += UmHypot(-3, 4) != 5;
    bad += UmHypot(inf, 1) != inf;
    bad += UmHypot(1, -inf) != inf;
    bad += !std::isnan(UmHypot(nan, 1));
    bad += UmHypot(1e200, 1e200) == inf;      // would overflow without the slow path
    bad += UmHypot(1e-200, 1e-200) == 0;      // would underflow
    std::printf("random: worst error vs exact %.2f ulp, vs the game's algorithm %.2f ulp, differs in %.3f%% of %ld; special cases wrong: %d\n",
                worstRef, worstOrig, 100.0 * differ / total, total, bad);
    return (worstRef <= 1.0 && bad == 0) ? 0 : 1;
}
