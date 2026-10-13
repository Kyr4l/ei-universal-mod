// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Makes the AppImage run on distributions older than the build machine. A binary binds to the NEWEST
// version of each glibc symbol it uses, and recent glibc gave new versions to a few plain functions
// (2.35 hypotf; 2.38 fmod, fmodf and the C23 strtol / sscanf, which only add "0b" prefixes; 2.43 the
// correctly-rounded asinf, acosf, atan2f, log10f, sqrtf). Linked with -Wl,--wrap=<name> for each of
// them (Makefile, APPIMAGE_WRAPS), every call to <name> comes here instead and goes to the version the
// function has had since GLIBC_2.2.5: the same function, so the AppImage needs glibc 2.34 (2021) and no
// more. arc4random (2.36, used by libstdc++'s random_device) is replaced by getrandom (2.25).
// Plain C on purpose: without _GNU_SOURCE the headers do not rename strtol / sscanf themselves.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>

#define OLD_MATH1(name) \
    extern float name##_glibc225(float); \
    __asm__(".symver " #name "_glibc225, " #name "@GLIBC_2.2.5"); \
    float __wrap_##name(float x) { return name##_glibc225(x); }
#define OLD_MATH2(name, T) \
    extern T name##_glibc225(T, T); \
    __asm__(".symver " #name "_glibc225, " #name "@GLIBC_2.2.5"); \
    T __wrap_##name(T x, T y) { return name##_glibc225(x, y); }

OLD_MATH1(asinf)
OLD_MATH1(acosf)
OLD_MATH1(log10f)
OLD_MATH1(sqrtf)
OLD_MATH2(atan2f, float)
OLD_MATH2(hypotf, float)
OLD_MATH2(fmodf, float)
OLD_MATH2(fmod, double)

long __wrap___isoc23_strtol(const char* s, char** end, int base) { return strtol(s, end, base); }
unsigned long __wrap___isoc23_strtoul(const char* s, char** end, int base) { return strtoul(s, end, base); }
int __wrap___isoc23_sscanf(const char* s, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int n = vsscanf(s, format, args);
    va_end(args);
    return n;
}
uint32_t __wrap_arc4random(void) {
    uint32_t v = 0;
    while (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v) {}
    return v;
}
