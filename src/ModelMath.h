#pragma once

// Matrix helpers for model exchange (ModelExchange.cpp / ModelImport.cpp). glTF convention: column vectors, column-major storage
// (m[col * 4 + row]); the game's row-vector matrices are mirrored into it (z flipped, see ModelExchange.h).

#include "geofile.h"

#include <cmath>

namespace modelmath {

struct M4 {
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    double& at(int row, int col) { return m[col * 4 + row]; }
    double at(int row, int col) const { return m[col * 4 + row]; }
};

inline M4 operator*(const M4& a, const M4& b) {
    M4 r;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a.at(row, k) * b.at(k, col);
            r.at(row, col) = s;
        }
    return r;
}

inline M4 fromTrs(const double t[3], const double q[4], const double s[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r;
    r.at(0, 0) = (1 - 2 * (y * y + z * z)) * s[0];
    r.at(1, 0) = (2 * (x * y + z * w)) * s[0];
    r.at(2, 0) = (2 * (x * z - y * w)) * s[0];
    r.at(0, 1) = (2 * (x * y - z * w)) * s[1];
    r.at(1, 1) = (1 - 2 * (x * x + z * z)) * s[1];
    r.at(2, 1) = (2 * (y * z + x * w)) * s[1];
    r.at(0, 2) = (2 * (x * z + y * w)) * s[2];
    r.at(1, 2) = (2 * (y * z - x * w)) * s[2];
    r.at(2, 2) = (1 - 2 * (x * x + y * y)) * s[2];
    r.at(0, 3) = t[0];
    r.at(1, 3) = t[1];
    r.at(2, 3) = t[2];
    return r;
}

inline M4 inverse(const M4& a) {
    const double* m = a.m;
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    M4 r;
    if (std::fabs(det) < 1e-30) return r;
    det = 1.0 / det;
    for (int k = 0; k < 16; ++k) r.m[k] = inv[k] * det;
    return r;
}

struct Trs {
    tr::Vec3 translation;
    tr::Quat rotation;
    tr::Vec3 scale{1, 1, 1};
};

inline Trs decompose(const M4& a) {
    Trs out;
    out.translation = tr::Vec3{float(a.at(0, 3)), float(a.at(1, 3)), float(a.at(2, 3))};
    double col[3][3];
    double s[3];
    for (int c = 0; c < 3; ++c) {
        s[c] = std::sqrt(a.at(0, c) * a.at(0, c) + a.at(1, c) * a.at(1, c) + a.at(2, c) * a.at(2, c));
        for (int r = 0; r < 3; ++r) col[c][r] = s[c] > 1e-20 ? a.at(r, c) / s[c] : (r == c ? 1 : 0);
    }
    const double det = col[0][0] * (col[1][1] * col[2][2] - col[2][1] * col[1][2]) - col[1][0] * (col[0][1] * col[2][2] - col[2][1] * col[0][2]) +
                       col[2][0] * (col[0][1] * col[1][2] - col[1][1] * col[0][2]);
    if (det < 0) {
        s[0] = -s[0];
        for (int r = 0; r < 3; ++r) col[0][r] = -col[0][r];
    }
    out.scale = tr::Vec3{float(s[0]), float(s[1]), float(s[2])};
    const double m00 = col[0][0], m11 = col[1][1], m22 = col[2][2];
    const double trace = m00 + m11 + m22;
    double x, y, z, w;
    if (trace > 0) {
        const double k = std::sqrt(trace + 1.0) * 2;
        w = 0.25 * k;
        x = (col[1][2] - col[2][1]) / k;
        y = (col[2][0] - col[0][2]) / k;
        z = (col[0][1] - col[1][0]) / k;
    } else if (m00 > m11 && m00 > m22) {
        const double k = std::sqrt(1.0 + m00 - m11 - m22) * 2;
        w = (col[1][2] - col[2][1]) / k;
        x = 0.25 * k;
        y = (col[1][0] + col[0][1]) / k;
        z = (col[2][0] + col[0][2]) / k;
    } else if (m11 > m22) {
        const double k = std::sqrt(1.0 + m11 - m00 - m22) * 2;
        w = (col[2][0] - col[0][2]) / k;
        x = (col[1][0] + col[0][1]) / k;
        y = 0.25 * k;
        z = (col[2][1] + col[1][2]) / k;
    } else {
        const double k = std::sqrt(1.0 + m22 - m00 - m11) * 2;
        w = (col[0][1] - col[1][0]) / k;
        x = (col[2][0] + col[0][2]) / k;
        y = (col[2][1] + col[1][2]) / k;
        z = 0.25 * k;
    }
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    out.rotation = tr::Quat{float(x / n), float(y / n), float(z / n), float(w / n)};
    return out;
}

inline tr::Vec3 mirror(const tr::Vec3& v) { return tr::Vec3{v.x, v.y, -v.z}; }
inline tr::Quat mirror(const tr::Quat& q) { return tr::Quat{-q.x, -q.y, q.z, q.w}; }

// A game local transform (translation, rotation, scale) as a glTF-space matrix (mirrored).
inline M4 gltfTrs(const tr::Vec3& translation, const tr::Quat& rotation, const tr::Vec3& scale) {
    const tr::Vec3 t = mirror(translation);
    const tr::Quat q = mirror(rotation);
    const double td[3] = {t.x, t.y, t.z}, qd[4] = {q.x, q.y, q.z, q.w}, sd[3] = {scale.x, scale.y, scale.z};
    return fromTrs(td, qd, sd);
}

inline M4 gltfLocal(const tr::GeoFileBone& b) { return gltfTrs(b.translation, b.rotation, b.scale); }

// glTF column-major array <-> game row-vector row-major array: the same entries, those with exactly one index on z negated.
inline void mirrorArray(const double (&in)[16], float (&out)[16]) {
    for (int a = 0; a < 16; ++a) {
        const int col = a / 4, row = a % 4;
        out[a] = float(((row == 2) != (col == 2)) ? -in[a] : in[a]);
    }
}

inline M4 gltfFromGameArray(const float (&in)[16]) {
    M4 r;
    for (int a = 0; a < 16; ++a) {
        const int col = a / 4, row = a % 4;
        r.m[a] = ((row == 2) != (col == 2)) ? -double(in[a]) : double(in[a]);
    }
    return r;
}

inline tr::Vec3 transformPoint(const M4& m, const tr::Vec3& v) {
    return tr::Vec3{float(m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z + m.at(0, 3)),
                    float(m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z + m.at(1, 3)),
                    float(m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z + m.at(2, 3))};
}

inline tr::Vec3 transformNormal(const M4& m, const tr::Vec3& v) {
    const M4 inv = inverse(m);
    tr::Vec3 r{float(inv.at(0, 0) * v.x + inv.at(1, 0) * v.y + inv.at(2, 0) * v.z), float(inv.at(0, 1) * v.x + inv.at(1, 1) * v.y + inv.at(2, 1) * v.z),
               float(inv.at(0, 2) * v.x + inv.at(1, 2) * v.y + inv.at(2, 2) * v.z)};
    const float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    return n > 1e-12f ? tr::Vec3{r.x / n, r.y / n, r.z / n} : tr::Vec3{0, 1, 0};
}

// Bind (PSKE) world matrices of every bone in glTF space (parents come first in shipped files; a later parent is treated as none).
inline std::vector<M4> bindWorlds(const std::vector<tr::GeoFileBone>& bones) {
    std::vector<M4> w(bones.size());
    for (size_t b = 0; b < bones.size(); ++b) {
        const int32_t p = bones[b].parent;
        w[b] = (p >= 0 && size_t(p) < b ? w[size_t(p)] : M4{}) * gltfLocal(bones[b]);
    }
    return w;
}

// Rest world matrices from the inverse binds (the pose the mesh is bound in; what Blender builds its rest pose from); a bone
// without one uses its bind world.
inline std::vector<M4> restWorlds(const std::vector<tr::GeoFileBone>& bones) {
    const std::vector<M4> bind = bindWorlds(bones);
    std::vector<M4> w(bones.size());
    for (size_t b = 0; b < bones.size(); ++b) w[b] = bones[b].hasInverseBind ? inverse(gltfFromGameArray(bones[b].inverseBind)) : bind[b];
    return w;
}

}  // namespace modelmath
