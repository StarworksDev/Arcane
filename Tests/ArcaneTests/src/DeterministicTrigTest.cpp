// DeterministicTrig: the bit-exact tables below run on every CI leg (MSVC x64,
// gcc-14 / clang-19 x64 Linux, AppleClang arm64), so a green table on all of
// them IS the cross-platform proof that the 2D <-> quaternion bridge produces
// the same bits everywhere (3D-readiness PR #20). The expected values were
// computed once by the implementation's method, mirrored in an independent
// exact-arithmetic model (Python: IEEE doubles, an exact fma, the same
// constants), and every finite sin/cos/atan2 entry was checked against a
// 700-bit fixed-point reference: each one is the correctly rounded float.
// Never edit a row to make it pass; a differing row is a determinism bug.
#include <Arcane/Math/DeterministicTrig.hpp>
#include <Arcane/Scene/Components.hpp>

#include <glm/gtc/quaternion.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace
{
    namespace DT = Arcane::DeterministicTrig;

    constexpr float kInf = std::numeric_limits<float>::infinity();

    std::uint32_t Bits(float f) { return std::bit_cast<std::uint32_t>(f); }

    // Distance in representable floats: 0 = the same value (+0 and -0 both
    // map to 0), 1 = adjacent floats.
    std::int64_t UlpDistance(float a, float b)
    {
        const auto ordered = [](float f) -> std::int64_t {
            const std::int64_t u = std::bit_cast<std::uint32_t>(f);
            return (u & 0x80000000) ? -(u & 0x7fffffff) : u;
        };
        const std::int64_t d = ordered(a) - ordered(b);
        return d < 0 ? -d : d;
    }

    struct SinCosRow { float x, s, c; };
    struct Atan2Row  { float y, x, a; };
    struct QuatRow   { float radians, w, z; };
    struct AngleRow  { float w, x, y, z, angle; };

    // { x, sin(x), cos(x) }: zeros, subnormals, the pi/4 and pi/2 seams and
    // their neighbours, +-pi, multiples of pi/2, large and huge magnitudes
    // (FLT_MAX), and a near-multiple of pi/2 that stresses the reduction.
    const SinCosRow kSinCos[] = {
        { 0x0p+0f, 0x0p+0f, 0x1p+0f },
        { -0x0p+0f, -0x0p+0f, 0x1p+0f },
        { 0x1p-149f, 0x1p-149f, 0x1p+0f },
        { -0x1p-149f, -0x1p-149f, 0x1p+0f },
        { 0x1p-126f, 0x1p-126f, 0x1p+0f },
        { 0x1.79ca1p-67f, 0x1.79ca1p-67f, 0x1p+0f },
        { 0x1p-12f, 0x1p-12f, 0x1p+0f },
        { 0x1p-1f, 0x1.eaee88p-2f, 0x1.c1528p-1f },
        { 0x1p+0f, 0x1.aed548p-1f, 0x1.14a28p-1f },
        { -0x1p+0f, -0x1.aed548p-1f, 0x1.14a28p-1f },
        { 0x1.921fb4p-1f, 0x1.6a09e6p-1f, 0x1.6a09e8p-1f },
        { 0x1.921fb6p-1f, 0x1.6a09e6p-1f, 0x1.6a09e6p-1f },
        { 0x1.921fb8p-1f, 0x1.6a09e8p-1f, 0x1.6a09e4p-1f },
        { 0x1.921fb6p+0f, 0x1p+0f, -0x1.777a5cp-25f },
        { -0x1.921fb6p+0f, -0x1p+0f, -0x1.777a5cp-25f },
        { 0x1.921fb4p+0f, 0x1p+0f, 0x1.4442d2p-24f },
        { 0x1.921fb6p+1f, -0x1.777a5cp-24f, -0x1p+0f },
        { -0x1.921fb6p+1f, 0x1.777a5cp-24f, -0x1p+0f },
        { 0x1.921fb4p+1f, 0x1.4442d2p-23f, -0x1p+0f },
        { 0x1.2d97c8p+2f, -0x1p+0f, 0x1.99bc5cp-27f },
        { 0x1.921fb6p+2f, 0x1.777a5cp-23f, 0x1p+0f },
        { -0x1.921fb6p+2f, -0x1.777a5cp-23f, 0x1p+0f },
        { 0x1p+2f, -0x1.837b9ep-1f, -0x1.4eaa6p-1f },
        { 0x1.4p+3f, -0x1.1689fp-1f, -0x1.ad9ac8p-1f },
        { 0x1.9p+6f, -0x1.03425cp-1f, 0x1.b981dcp-1f },
        { -0x1.f44p+9f, -0x1.fd948cp-1f, 0x1.8dbff8p-4f },
        { 0x1.86ap+16f, 0x1.24daaap-5f, -0x1.ffac38p-1f },
        { 0x1p+24f, -0x1.8f22f8p-1f, 0x1.40ad68p-1f },
        { 0x1.2a05f2p+33f, -0x1.f334c8p-2f, 0x1.bf098ap-1f },
        { 0x1.03a19ep+39f, 0x1.020eeep-1f, 0x1.ba35d4p-1f },
        { 0x1p+100f, -0x1.be8edap-1f, 0x1.f4eb4p-2f },
        { 0x1.fffffep+127f, -0x1.0b3366p-1f, 0x1.b4bf2cp-1f },
        { -0x1.fffffep+127f, 0x1.0b3366p-1f, 0x1.b4bf2cp-1f },
        { 0x1.666666p-2f, 0x1.5f209ap-2f, 0x1.e0f576p-1f },
        { -0x1.6p+1f, -0x1.86d224p-2f, -0x1.d93e2ap-1f },
        { 0x1.8p+1f, 0x1.210386p-3f, -0x1.fae04cp-1f },
        { -0x1.8cccccp+1f, -0x1.54a12cp-5f, -0x1.ff8ea4p-1f },
    };

    // { y, x, atan2(y, x) }: every signed-zero and infinity rule of
    // std::atan2, all four quadrants, the kernel's breakpoints, tiny and huge
    // ratios.
    const Atan2Row kAtan2[] = {
        { 0x0p+0f, 0x1p+0f, 0x0p+0f },
        { -0x0p+0f, 0x1p+0f, -0x0p+0f },
        { 0x0p+0f, -0x1p+0f, 0x1.921fb6p+1f },
        { -0x0p+0f, -0x1p+0f, -0x1.921fb6p+1f },
        { 0x0p+0f, 0x0p+0f, 0x0p+0f },
        { -0x0p+0f, 0x0p+0f, -0x0p+0f },
        { 0x0p+0f, -0x0p+0f, 0x1.921fb6p+1f },
        { -0x0p+0f, -0x0p+0f, -0x1.921fb6p+1f },
        { 0x1p+0f, 0x0p+0f, 0x1.921fb6p+0f },
        { -0x1p+0f, 0x0p+0f, -0x1.921fb6p+0f },
        { 0x1p+0f, -0x0p+0f, 0x1.921fb6p+0f },
        { -0x1p+0f, -0x0p+0f, -0x1.921fb6p+0f },
        { 0x1p+0f, 0x1p+0f, 0x1.921fb6p-1f },
        { 0x1p+0f, -0x1p+0f, 0x1.2d97c8p+1f },
        { -0x1p+0f, -0x1p+0f, -0x1.2d97c8p+1f },
        { -0x1p+0f, 0x1p+0f, -0x1.921fb6p-1f },
        { 0x1.0d9fd4p-3f, 0x1p+0f, 0x1.0c1524p-3f },
        { 0x1.a8279ap-2f, 0x1p+0f, 0x1.921fb6p-2f },
        { 0x1.88df16p-1f, 0x1p+0f, 0x1.4f1a6cp-1f },
        { 0x1.ffffdep-1f, 0x1p+0f, 0x1.921fa4p-1f },
        { 0x1p+0f, 0x1.88df16p-1f, 0x1.d524fep-1f },
        { 0x1.8p+1f, 0x1p+2f, 0x1.4978fap-1f },
        { -0x1p+2f, 0x1.8p+1f, -0x1.dac67p-1f },
        { -0x1.4p+1f, 0x1.dp+2f, -0x1.540766p-2f },
        { 0x1.dp+2f, -0x1.4p+1f, 0x1.e7218ep+0f },
        { 0x1p+0f, 0x1.4484cp-100f, 0x1.921fb6p+0f },
        { 0x1.4484cp-100f, 0x1p+0f, 0x1.4484cp-100f },
        { 0x1p-149f, 0x1p+0f, 0x1p-149f },
        { -0x1p-149f, -0x1p+0f, -0x1.921fb6p+1f },
        { 0x1p+0f, 0x1p-149f, 0x1.921fb6p+0f },
        { 0x1.fffffep+127f, 0x1.fffffep+127f, 0x1.921fb6p-1f },
        { 0x1.fffffep+127f, 0x1p-149f, 0x1.921fb6p+0f },
        { 0x1p-149f, 0x1.fffffep+127f, 0x0p+0f },
        { kInf, 0x1p+0f, 0x1.921fb6p+0f },
        { -kInf, 0x1p+0f, -0x1.921fb6p+0f },
        { kInf, kInf, 0x1.921fb6p-1f },
        { kInf, -kInf, 0x1.2d97c8p+1f },
        { -kInf, -kInf, -0x1.2d97c8p+1f },
        { 0x1p+0f, kInf, 0x0p+0f },
        { 0x1p+0f, -kInf, 0x1.921fb6p+1f },
        { -0x1p+0f, -kInf, -0x1.921fb6p+1f },
        { -0x0p+0f, -kInf, -0x1.921fb6p+1f },
    };

    // { radians, w, z } of QuatAboutZ; x and y must be +0.
    const QuatRow kQuat[] = {
        { 0x0p+0f, 0x1p+0f, 0x0p+0f },
        { -0x0p+0f, 0x1p+0f, -0x0p+0f },
        { 0x1.ad7f2ap-24f, 0x1p+0f, 0x1.ad7f2ap-25f },
        { 0x1.666666p-2f, 0x1.f82e14p-1f, 0x1.6492cep-3f },
        { 0x1p+0f, 0x1.c1528p-1f, 0x1.eaee88p-2f },
        { -0x1p+0f, 0x1.c1528p-1f, -0x1.eaee88p-2f },
        { 0x1.921fb6p+0f, 0x1.6a09e6p-1f, 0x1.6a09e6p-1f },
        { -0x1.921fb6p+0f, 0x1.6a09e6p-1f, -0x1.6a09e6p-1f },
        { 0x1.4p+1f, 0x1.42e3dep-2f, 0x1.e5e15p-1f },
        { 0x1.921fb6p+1f, -0x1.777a5cp-25f, 0x1p+0f },
        { -0x1.921fb6p+1f, -0x1.777a5cp-25f, -0x1p+0f },
        { -0x1.8cccccp+1f, 0x1.54b408p-6f, -0x1.ffe3a8p-1f },
        { 0x1.9p+6f, 0x1.ee1006p-1f, -0x1.0cabfep-2f },
        { -0x1.e24p+16f, -0x1.9e808p-2f, -0x1.d42daep-1f },
    };

    // { w, x, y, z, AngleAboutZ }: identity, the two half turns, non-planar
    // quaternions (the angle of the projected +X image), and QuatAboutZ
    // outputs -- the last float-pi row wraps to the negative side because
    // float(pi) lies above pi.
    const AngleRow kAngle[] = {
        { 0x1p+0f, 0x0p+0f, 0x0p+0f, 0x0p+0f, 0x0p+0f },
        { 0x0p+0f, 0x0p+0f, 0x0p+0f, 0x1p+0f, 0x1.921fb6p+1f },
        { 0x0p+0f, 0x0p+0f, 0x0p+0f, -0x1p+0f, 0x1.921fb6p+1f },
        { 0x1p-1f, 0x1p-1f, 0x1p-1f, 0x1p-1f, 0x1.921fb6p+0f },
        { 0x1.99999ap-1f, 0x1.333334p-1f, 0x0p+0f, 0x0p+0f, 0x0p+0f },
        { 0x1.99999ap-1f, 0x0p+0f, 0x1.333334p-1f, 0x0p+0f, 0x0p+0f },
        { 0x1p-1f, -0x1p-1f, 0x1p-1f, -0x1p-1f, -0x1.921fb6p+0f },
        { 0x1.f82e14p-1f, 0x0p+0f, 0x0p+0f, 0x1.6492cep-3f, 0x1.666666p-2f },
        { 0x1.c1528p-1f, 0x0p+0f, 0x0p+0f, 0x1.eaee88p-2f, 0x1p+0f },
        { 0x1.42e3dep-2f, 0x0p+0f, 0x0p+0f, 0x1.e5e15p-1f, 0x1.4p+1f },
        { 0x1.54b408p-6f, 0x0p+0f, 0x0p+0f, -0x1.ffe3a8p-1f, -0x1.8cccccp+1f },
        { -0x1.777a5cp-25f, 0x0p+0f, 0x0p+0f, 0x1p+0f, -0x1.921fb4p+1f },
        { 0x1p+0f, 0x0p+0f, 0x0p+0f, 0x1.ad7f2ap-25f, 0x1.ad7f2ap-24f },
    };

    // A finite float drawn uniformly over BIT PATTERNS (every exponent equally
    // likely), from a fixed-seed mt19937 (its output sequence is specified by
    // the standard, unlike the distributions).
    float RandomFiniteFloat(std::mt19937& rng)
    {
        for (;;)
        {
            const float f = std::bit_cast<float>(static_cast<std::uint32_t>(rng()));
            if (std::isfinite(f)) return f;
        }
    }
}

TEST_CASE("DeterministicTrig: sin and cos match the bit-exact table", "[math][determinism]")
{
    for (const SinCosRow& r : kSinCos)
    {
        INFO("x = " << r.x << " (0x" << std::hex << Bits(r.x) << ")");
        CHECK(Bits(DT::Sin(r.x)) == Bits(r.s));
        CHECK(Bits(DT::Cos(r.x)) == Bits(r.c));
        const DT::SinCosResult sc = DT::SinCos(r.x);
        CHECK(Bits(sc.sin) == Bits(r.s));
        CHECK(Bits(sc.cos) == Bits(r.c));
    }
}

TEST_CASE("DeterministicTrig: atan2 matches the bit-exact table", "[math][determinism]")
{
    for (const Atan2Row& r : kAtan2)
    {
        INFO("y = " << r.y << " (0x" << std::hex << Bits(r.y) << "), x = " << r.x << " (0x" << Bits(r.x) << ")");
        CHECK(Bits(DT::Atan2(r.y, r.x)) == Bits(r.a));
    }
}

TEST_CASE("DeterministicTrig: the planar bridge matches the bit-exact table", "[math][determinism]")
{
    for (const QuatRow& r : kQuat)
    {
        INFO("radians = " << r.radians << " (0x" << std::hex << Bits(r.radians) << ")");
        const glm::quat q = DT::QuatAboutZ(r.radians);
        CHECK(Bits(q.w) == Bits(r.w));
        CHECK(Bits(q.x) == 0u);
        CHECK(Bits(q.y) == 0u);
        CHECK(Bits(q.z) == Bits(r.z));
    }
    for (const AngleRow& r : kAngle)
    {
        const glm::quat q(r.w, r.x, r.y, r.z);   // glm's ctor order is (w, x, y, z)
        INFO("q = (" << r.w << ", " << r.x << ", " << r.y << ", " << r.z << ")");
        CHECK(Bits(DT::AngleAboutZ(q)) == Bits(r.angle));
    }
}

TEST_CASE("DeterministicTrig: Components' RotationAboutZ / RotationZ are the deterministic bridge", "[math][determinism]")
{
    std::mt19937 rng(20261023u);
    for (int i = 0; i < 4096; ++i)
    {
        const float a = RandomFiniteFloat(rng);
        const glm::quat viaScene = Arcane::RotationAboutZ(a);
        const glm::quat viaCore  = DT::QuatAboutZ(a);
        REQUIRE(Bits(viaScene.w) == Bits(viaCore.w));
        REQUIRE(Bits(viaScene.x) == Bits(viaCore.x));
        REQUIRE(Bits(viaScene.y) == Bits(viaCore.y));
        REQUIRE(Bits(viaScene.z) == Bits(viaCore.z));
        const glm::quat q = glm::normalize(glm::quat(RandomFiniteFloat(rng), 0.3f, -0.2f, 0.9f));
        if (std::isfinite(q.w))
            REQUIRE(Bits(Arcane::RotationZ(q)) == Bits(DT::AngleAboutZ(q)));
    }
}

TEST_CASE("DeterministicTrig: non-finite input gives the one canonical NaN", "[math][determinism]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr std::uint32_t kCanonicalNaN = 0x7fc00000u;
    for (const float x : { nan, -nan, kInf, -kInf })
    {
        CHECK(Bits(DT::Sin(x)) == kCanonicalNaN);
        CHECK(Bits(DT::Cos(x)) == kCanonicalNaN);
        CHECK(Bits(DT::SinCos(x).sin) == kCanonicalNaN);
        CHECK(Bits(DT::SinCos(x).cos) == kCanonicalNaN);
    }
    CHECK(Bits(DT::Atan2(nan, 1.0f)) == kCanonicalNaN);
    CHECK(Bits(DT::Atan2(1.0f, -nan)) == kCanonicalNaN);
    CHECK(Bits(DT::Atan2(nan, nan)) == kCanonicalNaN);
    CHECK(std::isnan(DT::AngleAboutZ(glm::quat(nan, 0.0f, 0.0f, 0.0f))));
    const glm::quat q = DT::QuatAboutZ(kInf);
    CHECK(Bits(q.w) == kCanonicalNaN);
    CHECK(Bits(q.z) == kCanonicalNaN);
}

TEST_CASE("DeterministicTrig: within 1 ULP of the double-precision libm everywhere", "[math][determinism]")
{
    // The reference is the platform's DOUBLE sin/cos/atan2 rounded to float:
    // its own error (< 1 double ULP) is ~2^-29 float ULPs, so a distance of 1
    // here is the bound a correctly rounded float can show against it.
    std::int64_t worstSin = 0, worstCos = 0, worstAtan2 = 0;

    // Dense over the engine's range, [-pi, pi] and a little beyond.
    constexpr int kDense = 200000;
    for (int i = 0; i <= kDense; ++i)
    {
        const float x = static_cast<float>(-3.5 + 7.0 * static_cast<double>(i) / kDense);
        worstSin = std::max(worstSin, UlpDistance(DT::Sin(x), static_cast<float>(std::sin(static_cast<double>(x)))));
        worstCos = std::max(worstCos, UlpDistance(DT::Cos(x), static_cast<float>(std::cos(static_cast<double>(x)))));
    }

    // Every exponent, both signs, the whole finite range.
    std::mt19937 rng(0xA7C0FFEEu);
    for (int i = 0; i < 200000; ++i)
    {
        const float x = RandomFiniteFloat(rng);
        worstSin = std::max(worstSin, UlpDistance(DT::Sin(x), static_cast<float>(std::sin(static_cast<double>(x)))));
        worstCos = std::max(worstCos, UlpDistance(DT::Cos(x), static_cast<float>(std::cos(static_cast<double>(x)))));
        const float y = RandomFiniteFloat(rng);
        worstAtan2 = std::max(worstAtan2,
            UlpDistance(DT::Atan2(y, x), static_cast<float>(std::atan2(static_cast<double>(y), static_cast<double>(x)))));
    }

    // atan2 over ordinary magnitudes in every quadrant.
    // Raw generator output, not a std:: distribution: the inputs are then the
    // same on every standard library.
    const auto grid = [&rng] { return static_cast<int>(rng() % 8001u) - 4000; };
    for (int i = 0; i < 200000; ++i)
    {
        const float y = static_cast<float>(grid()) * 0.0025f;
        const float x = static_cast<float>(grid()) * 0.0025f;
        if (y == 0.0f && x == 0.0f) continue;
        worstAtan2 = std::max(worstAtan2,
            UlpDistance(DT::Atan2(y, x), static_cast<float>(std::atan2(static_cast<double>(y), static_cast<double>(x)))));
    }

    INFO("worst ULP distance: sin " << worstSin << ", cos " << worstCos << ", atan2 " << worstAtan2);
    CHECK(worstSin <= 1);
    CHECK(worstCos <= 1);
    CHECK(worstAtan2 <= 1);
}

TEST_CASE("DeterministicTrig: angle -> quaternion -> angle round-trips within 1 ULP", "[math][determinism]")
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kSteps = 400000;
    std::int64_t worst = 0;
    for (int i = 0; i <= kSteps; ++i)
    {
        const float a = static_cast<float>(-kPi + 2.0 * kPi * static_cast<double>(i) / kSteps);
        const float back = DT::AngleAboutZ(DT::QuatAboutZ(a));
        if (a == 0.0f) { CHECK(back == 0.0f); continue; }
        // Wrap the difference into (-pi, pi]: float(+-pi) lies outside [-pi, pi]
        // and legitimately comes back on the other side.
        double d = static_cast<double>(back) - static_cast<double>(a);
        if (d > kPi) d -= 2.0 * kPi;
        if (d < -kPi) d += 2.0 * kPi;
        const double ulp = static_cast<double>(std::nextafter(std::abs(a), kInf) - std::abs(a));
        worst = std::max(worst, static_cast<std::int64_t>(std::ceil(std::abs(d) / ulp - 1e-9)));
    }
    INFO("worst round-trip error: " << worst << " ULP");
    CHECK(worst <= 1);
}

TEST_CASE("DeterministicTrig: AngleAboutZ reads the image of local +X (glm's q * X)", "[math][determinism]")
{
    // The Components.hpp contract: RotationZ is atan2 of the rotated +X, so a
    // non-planar quaternion projects onto the XY plane. Compared against glm's
    // own float rotation with a tolerance (glm is the libm-dependent path).
    std::mt19937 rng(77u);
    const auto comp = [&rng] { return static_cast<int>(rng() % 2001u) - 1000; };
    int checked = 0;
    for (int i = 0; i < 20000; ++i)
    {
        const float w = static_cast<float>(comp());   // one draw per statement: argument order is unspecified
        const float x = static_cast<float>(comp());
        const float y = static_cast<float>(comp());
        const float z = static_cast<float>(comp());
        glm::quat q(w, x, y, z);
        if (glm::length(q) < 1.0f) continue;
        q = glm::normalize(q);
        const glm::vec3 fwd = q * glm::vec3(1.0f, 0.0f, 0.0f);
        if (std::hypot(fwd.x, fwd.y) < 1e-2f) continue;   // the angle of a near-vertical +X is ill-conditioned
        double d = static_cast<double>(DT::AngleAboutZ(q)) - std::atan2(static_cast<double>(fwd.y), static_cast<double>(fwd.x));
        if (d > 3.14159265358979323846) d -= 2.0 * 3.14159265358979323846;
        if (d < -3.14159265358979323846) d += 2.0 * 3.14159265358979323846;
        CHECK(std::abs(d) < 1e-4);
        ++checked;
    }
    CHECK(checked > 10000);
}
