// Minimal orbit camera + 4x4 matrix math for the fixed-function OpenGL2 renderer.
// Matrices are column-major float[16], matching glLoadMatrixf's expected layout.
#pragma once

#include <cmath>
#include <cstring>

struct Mat4 {
    float m[16];

    static Mat4 Identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    static Mat4 Perspective(float fovYDeg, float aspect, float nearZ, float farZ) {
        Mat4 r{};
        float f = 1.0f / std::tan(fovYDeg * 3.14159265f / 180.0f / 2.0f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (farZ + nearZ) / (nearZ - farZ);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        return r;
    }

    // Right-handed look-at, Z-up (matches the engine's own convention where
    // figure geometry stores Z as vertical - see figure-format.md).
    static Mat4 LookAt(float eyeX, float eyeY, float eyeZ, float centerX, float centerY, float centerZ) {
        float fx = centerX - eyeX, fy = centerY - eyeY, fz = centerZ - eyeZ;
        float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (flen > 1e-6f) { fx /= flen; fy /= flen; fz /= flen; }

        // World up is +Z.
        float upx = 0.0f, upy = 0.0f, upz = 1.0f;
        // s = f x up
        float sx = fy * upz - fz * upy;
        float sy = fz * upx - fx * upz;
        float sz = fx * upy - fy * upx;
        float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (slen > 1e-6f) { sx /= slen; sy /= slen; sz /= slen; }
        // u = s x f
        float ux = sy * fz - sz * fy;
        float uy = sz * fx - sx * fz;
        float uz = sx * fy - sy * fx;

        Mat4 r = Identity();
        r.m[0] = sx; r.m[4] = sy; r.m[8] = sz;
        r.m[1] = ux; r.m[5] = uy; r.m[9] = uz;
        r.m[2] = -fx; r.m[6] = -fy; r.m[10] = -fz;
        r.m[12] = -(sx * eyeX + sy * eyeY + sz * eyeZ);
        r.m[13] = -(ux * eyeX + uy * eyeY + uz * eyeZ);
        r.m[14] = -(-fx * eyeX + -fy * eyeY + -fz * eyeZ);
        return r;
    }
};

struct OrbitCamera {
    float yawDeg = 45.0f;
    float pitchDeg = 20.0f;
    float distance = 3.0f;
    float targetX = 0.0f, targetY = 0.0f, targetZ = 0.9f; // roughly chest height

    void Orbit(float dYaw, float dPitch) {
        yawDeg += dYaw;
        pitchDeg += dPitch;
        if (pitchDeg > 89.0f) pitchDeg = 89.0f;
        if (pitchDeg < -89.0f) pitchDeg = -89.0f;
    }

    void Zoom(float delta) {
        distance -= delta;
        if (distance < 0.2f) distance = 0.2f;
        if (distance > 50.0f) distance = 50.0f;
    }

    void Pan(float dx, float dy) {
        // Pan in the camera's local right/up plane, projected onto world X/Y (dy moves target Z).
        float yawRad = yawDeg * 3.14159265f / 180.0f;
        float rightX = std::cos(yawRad), rightY = -std::sin(yawRad);
        targetX += rightX * dx;
        targetY += rightY * dx;
        targetZ += dy;
    }

    void EyePosition(float& x, float& y, float& z) const {
        float yawRad = yawDeg * 3.14159265f / 180.0f;
        float pitchRad = pitchDeg * 3.14159265f / 180.0f;
        x = targetX + distance * std::cos(pitchRad) * std::cos(yawRad);
        y = targetY + distance * std::cos(pitchRad) * std::sin(yawRad);
        z = targetZ + distance * std::sin(pitchRad);
    }

    Mat4 ViewMatrix() const {
        float ex, ey, ez;
        EyePosition(ex, ey, ez);
        return Mat4::LookAt(ex, ey, ez, targetX, targetY, targetZ);
    }
};
