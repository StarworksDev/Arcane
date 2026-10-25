#pragma once

// DeterministicTrig -- sin, cos and atan2 that return the SAME BITS on every
// platform the engine ships on (MSVC x64, gcc/clang x64 Linux, AppleClang
// arm64), plus the planar quaternion bridge built on them. std::sin/cos/atan2
// come from each platform's libm (MSVC's CRT, glibc, Apple's libm), and those
// round differently: 1 ULP apart on the same input. That was enough to split
// the v1 physics step trace across platforms (3D-readiness PR #20,
// PhysicsV1BaselineTest:390), because the physics write-back turns the
// solver's angle into a quaternion and the trace reads it back.
//
// The definitions live in DeterministicTrig.cpp, compiled once into ArcaneCore
// under the engine's own floating-point flags, so the bits do not depend on
// the flags of whichever module calls them. The .cpp documents the method
// (exact integer range reduction, fixed polynomials, explicit fma only).
//
// Accuracy: within 1 ULP of the true value for EVERY finite input (in
// practice the correctly rounded float; the tests pin <= 1 ULP against a
// double-precision libm and a high-precision table). Non-finite input gives
// a NaN (bit pattern 0x7fc00000) for Sin/Cos; Atan2 follows std::atan2's
// IEEE-754 rules for zeros and infinities and returns 0x7fc00000 for a NaN.
//
// These are NOT fast-path replacements for std:: in rendering or the editor:
// use them wherever a value feeds the simulation, a replicated state or a
// recorded trace -- anything that must match across machines.
#include <Arcane/Core/Api.hpp>

#include <glm/gtc/quaternion.hpp>

namespace Arcane::DeterministicTrig
{
    struct SinCosResult
    {
        float sin = 0.0f;
        float cos = 1.0f;
    };

    [[nodiscard]] ARC_CORE_API float        Sin(float radians) noexcept;
    [[nodiscard]] ARC_CORE_API float        Cos(float radians) noexcept;
    [[nodiscard]] ARC_CORE_API SinCosResult SinCos(float radians) noexcept;   // == { Sin(x), Cos(x) }, one reduction
    [[nodiscard]] ARC_CORE_API float        Atan2(float y, float x) noexcept;

    // The planar bridge (Arcane::RotationAboutZ / Arcane::RotationZ in
    // Scene/Components.hpp forward here). QuatAboutZ is the unit quaternion
    // of a turn of `radians` about +Z: (w, x, y, z) = (cos(r/2), +0, +0,
    // sin(r/2)), with the half angle's sine and cosine evaluated once in
    // double and rounded to float. AngleAboutZ reads the image of local +X,
    // atan2(2(xy + wz), 1 - 2(y^2 + z^2)) -- the same formula as glm's
    // q * vec3(1, 0, 0) -- evaluated in double (each float product is exact
    // there) and rounded once. Round trip: AngleAboutZ(QuatAboutZ(a)) is a
    // (wrapped into [-pi, pi]) within 1 ULP of |a|; an input zero of either
    // sign comes back as +0.
    [[nodiscard]] ARC_CORE_API glm::quat QuatAboutZ(float radians) noexcept;
    [[nodiscard]] ARC_CORE_API float     AngleAboutZ(const glm::quat& q) noexcept;
}
