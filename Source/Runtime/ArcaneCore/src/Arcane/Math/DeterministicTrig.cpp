// DeterministicTrig (see the header for the contract). Every result is a pure
// function of the input BITS, computed with operations IEEE-754 specifies to
// be correctly rounded -- + - * / on doubles, std::fma, integer arithmetic,
// int64 -> double and double -> float conversion -- so MSVC x64, gcc/clang
// x64 and AppleClang arm64 produce identical bits. No libm transcendental is
// called, and nothing here depends on the caller's flags.
//
// THE METHOD
//   1. Work in DOUBLE, round to float once at the end. The double result is
//      within ~2 double ULPs of the true value, so the float is the correctly
//      rounded one except when the true value sits within ~2^-28 float ULPs of
//      a rounding midpoint -- and even then it is the same float everywhere,
//      because every step is deterministic.
//   2. sin/cos range reduction is EXACT integer arithmetic (Payne-Hanek): a
//      float is m * 2^e with a 24-bit m, and x * 2/pi mod 4 is formed from m
//      times a 128-bit window of 2/pi's bits (kTwoOverPi), so even FLT_MAX
//      reduces to the right quarter turn and a remainder accurate to ~2^-60.
//      Inputs with |x| <= pi/4 skip the reduction.
//   3. Fixed polynomials, fixed Horner order: Taylor series (coefficients
//      +-1/n!, written as the double nearest each) to degree 17 / 18 on
//      |r| <= pi/4 -- truncation error < 2^-63. atan: the argument t in
//      [0, 1] is shifted onto |u| <= tan(pi/24) about four centres
//      c_k ~ tan(k pi/12) with atan(t) = atan(c_k) + atan((t - c_k) / (1 + t c_k)),
//      then Taylor to degree 19 (error < 2^-65).
//
// FMA CONTRACTION -- why every multiply-add is an explicit std::fma. A
// compiler allowed to contract (AppleClang on arm64 and gcc/clang with -mfma
// default to it) may fuse `a * b + c` into one rounding or leave it as two,
// per expression and per optimisation level: different bits for the same
// source. The engine builds with -ffp-contract=off (premake5.lua) and MSVC's
// /fp:strict never fuses, but the guarantee should not hang on a flag a
// future build change could drop. So no `a * b + c` appears in this file:
// each one is std::fma, which IEEE-754 and the C++ standard define as ONE
// rounding of the exact a*b+c -- the same bits on all three toolchains
// (hardware fmadd/vfmadd, or the CRT's correctly rounded fma). A bare product
// only ever feeds a non-additive use (a multiplicand, std::fma's addend, a
// division) or is exact (the product of two floats is exact in double), so
// contraction has nothing left to change.
//
// Expected bits are pinned in Tests/ArcaneTests/src/DeterministicTrigTest.cpp.
#include <Arcane/Math/DeterministicTrig.hpp>

#include <Arcane/Core/Constant.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Arcane::DeterministicTrig
{
    namespace
    {
        ARC_CONSTANT("math identity: pi/2, pi/4 and pi, each the double nearest")
        constexpr double kPio2 = 0x1.921fb54442d18p+0, kPio4 = 0x1.921fb54442d18p-1, kPi = 0x1.921fb54442d18p+1;

        // floor(2/pi * 2^256), as eight 32-bit words, most significant first.
        // Words up to index 6 are read (FLT_MAX: e = 104, k0 = 3, words 3..6).
        ARC_CONSTANT("math identity: the leading 256 bits of 2/pi")
        constexpr std::uint32_t kTwoOverPi[8] = {
            0xa2f9836e, 0x4e441529, 0xfc2757d1, 0xf534ddc0, 0xdb629599, 0x3c439041, 0xfe5163ab, 0xdebbc561,
        };

        // sin r = r + r z (s3 + z (s5 + ... + z s17)), z = r^2; s_n = (-1)^((n-1)/2) / n!.
        ARC_CONSTANT("math identity: Taylor coefficients of sin, -1/3! .. +1/17!")
        constexpr double kSin[8] = {
            -0x1.5555555555555p-3, 0x1.1111111111111p-7, -0x1.a01a01a01a01ap-13, 0x1.71de3a556c734p-19,
            -0x1.ae64567f544e4p-26, 0x1.6124613a86d09p-33, -0x1.ae7f3e733b81fp-41, 0x1.952c77030ad4ap-49,
        };

        // cos r = 1 + z (c2 + z (c4 + ... + z c18)); c_n = (-1)^(n/2) / n!.
        ARC_CONSTANT("math identity: Taylor coefficients of cos, -1/2! .. -1/18!")
        constexpr double kCos[9] = {
            -0x1p-1, 0x1.5555555555555p-5, -0x1.6c16c16c16c17p-10, 0x1.a01a01a01a01ap-16,
            -0x1.27e4fb7789f5cp-22, 0x1.1eed8eff8d898p-29, -0x1.93974a8c07c9dp-37, 0x1.ae7f3e733b81fp-45,
            -0x1.6827863b97d97p-53,
        };

        // atan u = u + u z (a3 + z (a5 + ... + z a19)); a_n = (-1)^((n-1)/2) / n.
        ARC_CONSTANT("math identity: Taylor coefficients of atan, -1/3 .. -1/19")
        constexpr double kAtan[9] = {
            -0x1.5555555555555p-2, 0x1.999999999999ap-3, -0x1.2492492492492p-3, 0x1.c71c71c71c71cp-4,
            -0x1.745d1745d1746p-4, 0x1.3b13b13b13b14p-4, -0x1.1111111111111p-4, 0x1.e1e1e1e1e1e1ep-5,
            -0x1.af286bca1af28p-5,
        };

        // Centres c_k = the double nearest tan(k pi/12), k = 0..3, and atan(c_k)
        // OF THAT DOUBLE (computed to 700 bits, then rounded), so the identity
        // atan(t) = atan(c_k) + atan((t - c_k) / (1 + t c_k)) holds exactly for
        // the c_k actually used. Breakpoints: the doubles nearest tan((2k+1) pi/24),
        // so |u| <= tan(pi/24) ~ 0.1317 in every segment.
        ARC_CONSTANT("math identity: atan reduction centres tan(k pi/12)")
        constexpr double kAtanCentre[4] = { 0.0, 0x1.126145e9ecd56p-2, 0x1.279a74590331cp-1, 0x1p+0 };
        ARC_CONSTANT("math identity: atan of each reduction centre")
        constexpr double kAtanOfCentre[4] = { 0.0, 0x1.0c152382d7365p-2, 0x1.0c152382d7365p-1, 0x1.921fb54442d18p-1 };
        ARC_CONSTANT("math identity: atan segment breakpoints tan((2k+1) pi/24)")
        constexpr double kAtanBreak[3] = { 0x1.0d9fd31c98bf9p-3, 0x1.a827999fcef32p-2, 0x1.88df153d6a676p-1 };

        // 2^n for a normal double exponent n, built from its bits (exact, no libm).
        double Pow2(int n) noexcept
        {
            return std::bit_cast<double>(static_cast<std::uint64_t>(n + 1023) << 52);
        }

        struct Reduced
        {
            int    quadrant = 0;   // x = (4j + quadrant) * pi/2 + r
            double r        = 0.0; // |r| <= pi/4
        };

        // Exact reduction of |v| > pi/4 by pi/2. v is finite, positive and has at
        // most 24 significant bits (a float, or half of one), so v = m * 2^e with
        // an integer m < 2^24.
        Reduced ReduceQuarterTurns(double v) noexcept
        {
            const std::uint64_t bits = std::bit_cast<std::uint64_t>(v);
            const int biased = static_cast<int>((bits >> 52) & 0x7ffu);
            const std::uint64_t mantissa = (bits & ((std::uint64_t{1} << 52) - 1u)) | (std::uint64_t{1} << 52);
            const std::uint64_t m = mantissa >> 29;   // the low 29 bits are zero for a 24-bit significand
            const int e = biased - 1075 + 29;         // v = m * 2^e

            // v * 2/pi = sum_k m * w_k * 2^(e - 32(k+1)). A word whose weight
            // 2^(e - 32(k+1)) is >= 4 only adds multiples of 4 (whole turns): skip
            // the first k0 of them, then accumulate the next four words exactly.
            const int k0 = e < 34 ? 0 : (e - 2) / 32;
            std::uint64_t acc[6] = {};
            for (int j = 0; j < 4; ++j)
            {
                const std::uint64_t p = m * kTwoOverPi[k0 + j];   // < 2^56
                acc[3 - j] += p & 0xffffffffu;
                acc[4 - j] += p >> 32;
            }
            for (int i = 0; i < 5; ++i)
            {
                acc[i + 1] += acc[i] >> 32;
                acc[i] &= 0xffffffffu;
            }

            // acc (five 32-bit limbs, least significant first) * 2^-s == v * 2/pi
            // mod 4, with s = 32(k0 + 4) - e in [95, 154]: bits s and s+1 are the
            // quarter turn, the s bits below are the fraction. The dropped words
            // contribute < m * 2^-s < 2^-71 of a quarter turn.
            const int s = 32 * (k0 + 4) - e;
            const auto limb = [&acc](int i) -> std::uint64_t { return (i >= 0 && i < 5) ? acc[i] : 0u; };
            const auto chunk = [&limb](int offset) -> std::uint64_t {   // bits [offset, offset + 32), zero below bit 0
                const int index = offset >> 5;   // floor division: >> on a negative int is arithmetic (C++20)
                const int shift = offset & 31;
                std::uint64_t word = limb(index) >> shift;
                if (shift != 0) word |= limb(index + 1) << (32 - shift);
                return word & 0xffffffffu;
            };

            int quadrant = static_cast<int>(chunk(s) & 3u);
            std::uint64_t hi = (chunk(s - 32) << 32) | chunk(s - 64);   // the fraction, 128 bits
            std::uint64_t lo = (chunk(s - 96) << 32) | chunk(s - 128);
            bool negative = false;
            if ((hi >> 63) != 0)
            {
                // Fraction >= 1/2: round to the NEXT quarter turn; r = -(1 - fraction).
                lo = ~lo + 1u;
                hi = ~hi + (lo == 0 ? 1u : 0u);
                quadrant = (quadrant + 1) & 3;
                negative = true;
            }
            if (hi == 0 && lo == 0)
                return { quadrant, 0.0 };

            // Normalise, keep the top 63 bits (an int64 -> double conversion is
            // one correctly rounded step), scale by the exact power of two.
            const int lz = hi != 0 ? std::countl_zero(hi) : 64 + std::countl_zero(lo);
            std::uint64_t top = 0;
            if (lz == 0)       top = hi;
            else if (lz < 64)  top = (hi << lz) | (lo >> (64 - lz));
            else if (lz == 64) top = lo;
            else               top = lo << (lz - 64);
            const double fraction = static_cast<double>(static_cast<std::int64_t>(top >> 1)) * Pow2(-63 - lz);
            const double r = fraction * kPio2;
            return { quadrant, negative ? -r : r };
        }

        double KernelSin(double r) noexcept   // |r| <= pi/4
        {
            const double z = r * r;
            double p = kSin[7];
            for (int i = 6; i >= 0; --i) p = std::fma(p, z, kSin[i]);
            return std::fma(r * z, p, r);
        }

        double KernelCos(double r) noexcept   // |r| <= pi/4
        {
            const double z = r * r;
            double p = kCos[8];
            for (int i = 7; i >= 0; --i) p = std::fma(p, z, kCos[i]);
            return std::fma(z, p, 1.0);
        }

        struct SinCosD
        {
            double s = 0.0;
            double c = 1.0;
        };

        // v finite with <= 24 significant bits. Odd/even symmetry is exact:
        // the reduction runs on |v| and the sign is applied last.
        SinCosD SinCosKernel(double v) noexcept
        {
            const double a = std::fabs(v);
            const Reduced red = a <= kPio4 ? Reduced{ 0, a } : ReduceQuarterTurns(a);
            const double s = KernelSin(red.r);
            const double c = KernelCos(red.r);
            SinCosD out;
            switch (red.quadrant)
            {
            case 0:  out = { s, c };   break;
            case 1:  out = { c, -s };  break;
            case 2:  out = { -s, -c }; break;
            default: out = { -c, s };  break;
            }
            if (std::signbit(v)) out.s = -out.s;
            return out;
        }

        double Atan01(double t) noexcept   // t in [0, 1]
        {
            int k = 3;
            if (t <= kAtanBreak[0])      k = 0;
            else if (t <= kAtanBreak[1]) k = 1;
            else if (t <= kAtanBreak[2]) k = 2;
            const double c = kAtanCentre[k];
            const double u = (t - c) / std::fma(t, c, 1.0);
            const double z = u * u;
            double p = kAtan[8];
            for (int i = 7; i >= 0; --i) p = std::fma(p, z, kAtan[i]);
            return kAtanOfCentre[k] + std::fma(u * z, p, u);
        }

        // std::atan2's IEEE-754 cases for zeros and infinities; y and x not NaN.
        double Atan2Kernel(double y, double x) noexcept
        {
            const double ay = std::fabs(y);
            const double ax = std::fabs(x);
            const bool xNegative = std::signbit(x);
            double a = 0.0;
            if (ay == 0.0)
                a = xNegative ? kPi : 0.0;
            else if (std::isinf(ay) && std::isinf(ax))
                a = xNegative ? kPi - kPio4 : kPio4;
            else if (std::isinf(ay))
                a = kPio2;
            else if (std::isinf(ax))
                a = xNegative ? kPi : 0.0;
            else
            {
                a = ay <= ax ? Atan01(ay / ax) : kPio2 - Atan01(ax / ay);
                if (xNegative) a = kPi - a;
            }
            return std::copysign(a, y);
        }

        constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();   // 0x7fc00000 on every target
    }

    SinCosResult SinCos(float radians) noexcept
    {
        if (!std::isfinite(radians)) return { kNaN, kNaN };
        const SinCosD d = SinCosKernel(static_cast<double>(radians));
        return { static_cast<float>(d.s), static_cast<float>(d.c) };
    }

    float Sin(float radians) noexcept { return SinCos(radians).sin; }
    float Cos(float radians) noexcept { return SinCos(radians).cos; }

    float Atan2(float y, float x) noexcept
    {
        if (std::isnan(y) || std::isnan(x)) return kNaN;
        return static_cast<float>(Atan2Kernel(static_cast<double>(y), static_cast<double>(x)));
    }

    glm::quat QuatAboutZ(float radians) noexcept
    {
        if (!std::isfinite(radians)) return glm::quat(kNaN, 0.0f, 0.0f, kNaN);
        // Halving a float-valued double is exact (it only moves the exponent),
        // so the half angle still has <= 24 significant bits.
        const SinCosD half = SinCosKernel(static_cast<double>(radians) * 0.5);
        return glm::quat(static_cast<float>(half.c), 0.0f, 0.0f, static_cast<float>(half.s));   // (w, x, y, z)
    }

    float AngleAboutZ(const glm::quat& q) noexcept
    {
        // The image of +X under q: (1 - 2(y^2 + z^2), 2(xy + wz), ...). Each
        // product of two floats is exact in double, so these round once per
        // fma and are immune to contraction even before the explicit fma.
        const double w = q.w, x = q.x, y = q.y, z = q.z;
        const double forwardY = std::fma(x, y, w * z) * 2.0;
        const double forwardX = std::fma(-2.0, std::fma(y, y, z * z), 1.0);
        if (std::isnan(forwardY) || std::isnan(forwardX)) return kNaN;
        return static_cast<float>(Atan2Kernel(forwardY, forwardX));
    }
}
