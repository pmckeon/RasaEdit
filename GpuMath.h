#pragma once

// Small matrix library for the viewport. Column-vector convention (clip = projection * view * model * p),
// stored row by row, matching `row_major float4x4` in the shaders. World space is the game's
// left-handed space: +x right, +y up, +z forward. Clip-space depth is 0..1.

#include "trformats.h"

#include <cmath>

struct Vec4f {
    float x = 0, y = 0, z = 0, w = 0;
};

struct Mat4 {
    Vec4f row0, row1, row2, row3;
};

inline tr::Vec3 add(const tr::Vec3& a, const tr::Vec3& b) { return tr::Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline tr::Vec3 subtract(const tr::Vec3& a, const tr::Vec3& b) { return tr::Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline tr::Vec3 scaled(const tr::Vec3& a, float s) { return tr::Vec3{a.x * s, a.y * s, a.z * s}; }
inline float dot(const tr::Vec3& a, const tr::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline tr::Vec3 cross(const tr::Vec3& a, const tr::Vec3& b) {
    return tr::Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const tr::Vec3& a) { return std::sqrt(dot(a, a)); }
inline tr::Vec3 normalized(const tr::Vec3& a) {
    const float l = length(a);
    return l > 0 ? scaled(a, 1.0f / l) : a;
}

inline float rowDot(const Vec4f& row, float x, float y, float z, float w) { return row.x * x + row.y * y + row.z * z + row.w * w; }

inline Mat4 identityMatrix() {
    Mat4 m;
    m.row0 = {1, 0, 0, 0};
    m.row1 = {0, 1, 0, 0};
    m.row2 = {0, 0, 1, 0};
    m.row3 = {0, 0, 0, 1};
    return m;
}

inline Vec4f multiplyRow(const Vec4f& r, const Mat4& b) {
    return Vec4f{r.x * b.row0.x + r.y * b.row1.x + r.z * b.row2.x + r.w * b.row3.x,
                 r.x * b.row0.y + r.y * b.row1.y + r.z * b.row2.y + r.w * b.row3.y,
                 r.x * b.row0.z + r.y * b.row1.z + r.z * b.row2.z + r.w * b.row3.z,
                 r.x * b.row0.w + r.y * b.row1.w + r.z * b.row2.w + r.w * b.row3.w};
}

inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 m;
    m.row0 = multiplyRow(a.row0, b);
    m.row1 = multiplyRow(a.row1, b);
    m.row2 = multiplyRow(a.row2, b);
    m.row3 = multiplyRow(a.row3, b);
    return m;
}

inline tr::Vec3 transformPoint(const Mat4& m, const tr::Vec3& p) {
    return tr::Vec3{rowDot(m.row0, p.x, p.y, p.z, 1), rowDot(m.row1, p.x, p.y, p.z, 1), rowDot(m.row2, p.x, p.y, p.z, 1)};
}

// Left-handed perspective, Direct3D-style depth 0..1.
inline Mat4 perspectiveLH(float fovYRadians, float aspect, float nearZ, float farZ) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 m;
    m.row0 = {f / aspect, 0, 0, 0};
    m.row1 = {0, f, 0, 0};
    m.row2 = {0, 0, farZ / (farZ - nearZ), -nearZ * farZ / (farZ - nearZ)};
    m.row3 = {0, 0, 1, 0};
    return m;
}

// Left-handed view matrix from an eye position and orthonormal right / up / forward axes.
inline Mat4 viewLH(const tr::Vec3& eye, const tr::Vec3& right, const tr::Vec3& up, const tr::Vec3& forward) {
    Mat4 m;
    m.row0 = {right.x, right.y, right.z, -dot(right, eye)};
    m.row1 = {up.x, up.y, up.z, -dot(up, eye)};
    m.row2 = {forward.x, forward.y, forward.z, -dot(forward, eye)};
    m.row3 = {0, 0, 0, 1};
    return m;
}

// Rotation part of a unit quaternion (the map file stores it as w, x, y, z).
inline Mat4 rotationMatrix(const tr::Quat& q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 m;
    m.row0 = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0};
    m.row1 = {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0};
    m.row2 = {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0};
    m.row3 = {0, 0, 0, 1};
    return m;
}

// Placement of a map entity: translate * rotate * uniform scale.
inline Mat4 placementMatrix(const tr::Vec3& location, const tr::Quat& rotation, float scale) {
    Mat4 m = rotationMatrix(rotation);
    m.row0 = {m.row0.x * scale, m.row0.y * scale, m.row0.z * scale, location.x};
    m.row1 = {m.row1.x * scale, m.row1.y * scale, m.row1.z * scale, location.y};
    m.row2 = {m.row2.x * scale, m.row2.y * scale, m.row2.z * scale, location.z};
    return m;
}

// Maps the unit cube [0,1]^3 onto a box.
inline Mat4 boxMatrix(const tr::Bounds& b) {
    Mat4 m;
    m.row0 = {b.max.x - b.min.x, 0, 0, b.min.x};
    m.row1 = {0, b.max.y - b.min.y, 0, b.min.y};
    m.row2 = {0, 0, b.max.z - b.min.z, b.min.z};
    m.row3 = {0, 0, 0, 1};
    return m;
}
