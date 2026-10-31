// The EE FPU's float semantics, as the game's recompiled code computes them: a copy of the helpers in the port's
// third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h (GPL-3.0, as this repository), for host code that
// reproduces a game routine bit-exactly. Build with -ffp-contract=off (CMakeLists: the exe's float flags).
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <climits>
#include <immintrin.h>

// PS2 float semantics (EE FPU and VU0 macro mode). The PS2 format has no Inf, NaN or denormals:
//   - exponent 255 is an ordinary exponent, so 0x7F800000 is 2^128 and the largest value is 0x7FFFFFFF;
//   - exponent 0 is zero whatever the mantissa holds;
//   - a result of 2^129 or more saturates to +-0x7FFFFFFF, one below 2^-126 becomes +-0;
//   - x/0 gives +-0x7FFFFFFF.
// A host float cannot hold the exponent-255 values, so anything that touches them is computed in double.
// That path is rare and kept out of line; the inline part is the host operation and a range check.
// One case is left to the host: ADD.S/SUB.S/MUL.S and the vector operations do not look for an operand with
// exponent 0 and a non-zero mantissa, so it takes part with its tiny host value instead of as zero.
// Checked against the hardware results in ps2autotests (tests/cpu/ee_fpu).
#if defined(_MSC_VER)
#define PS2_FLOAT_NOINLINE __declspec(noinline)
#else
#define PS2_FLOAT_NOINLINE __attribute__((noinline))
#endif

constexpr uint32_t PS2_FLOAT_MAX_BITS = 0x7FFFFFFFu;
constexpr uint32_t PS2_FLOAT_EXP_MASK = 0x7F800000u;
constexpr uint32_t PS2_FLOAT_SIGN_MASK = 0x80000000u;

// True when a host float operation cannot be trusted with x: exponent 255, or exponent 0 with a mantissa.
inline bool ps2_fspecial(float x)
{
    const uint32_t t = std::bit_cast<uint32_t>(x) & 0x7FFFFFFFu;
    return t != 0u && (t - 0x00800000u) >= 0x7F000000u;
}

inline double ps2_to_double(float x)
{
    uint32_t b = std::bit_cast<uint32_t>(x);
    const uint32_t exponent = b & PS2_FLOAT_EXP_MASK;
    if (exponent == 0u)
        return static_cast<double>(std::bit_cast<float>(b & PS2_FLOAT_SIGN_MASK));
    if (exponent == PS2_FLOAT_EXP_MASK)
        return static_cast<double>(std::bit_cast<float>(b - 0x00800000u)) * 2.0;
    return static_cast<double>(x);
}

inline float ps2_from_double(double d)
{
    const double magnitude = std::fabs(d);
    const uint32_t sign = std::signbit(d) ? PS2_FLOAT_SIGN_MASK : 0u;
    if (magnitude >= 0x1p129)
        return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
    if (magnitude < 0x1p-126)
        return std::bit_cast<float>(sign);
    if (magnitude >= 0x1p128)
    {
        // Convert one exponent lower, then put the exponent back as 255.
        const uint32_t b = std::bit_cast<uint32_t>(static_cast<float>(d * 0.5));
        if ((b & PS2_FLOAT_EXP_MASK) == PS2_FLOAT_EXP_MASK)
            return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
        return std::bit_cast<float>(b + 0x00800000u);
    }
    // A value that rounds up to 2^128 comes out as the host's Inf pattern, which is 2^128 on the PS2.
    return static_cast<float>(d);
}

PS2_FLOAT_NOINLINE inline float ps2_fadd_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a + b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) + ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fsub_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a - b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) - ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fmul_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a * b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) * ps2_to_double(b));
}

// A zero divisor gives +-max with the sign of fs xor ft.
PS2_FLOAT_NOINLINE inline float ps2_fdiv_slow(float a, float b)
{
    const uint32_t ua = std::bit_cast<uint32_t>(a);
    const uint32_t ub = std::bit_cast<uint32_t>(b);
    if ((ub & PS2_FLOAT_EXP_MASK) == 0u)
        return std::bit_cast<float>(((ua ^ ub) & PS2_FLOAT_SIGN_MASK) | PS2_FLOAT_MAX_BITS);
    const float r = a / b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) / ps2_to_double(b));
}

// sqrt(|ft|); the result is never negative, sqrt(-0) is +0.
PS2_FLOAT_NOINLINE inline float ps2_fsqrt_slow(float a)
{
    if ((std::bit_cast<uint32_t>(a) & PS2_FLOAT_EXP_MASK) == 0u)
        return 0.0f;
    return ps2_from_double(std::sqrt(std::fabs(ps2_to_double(a))));
}

// fs / sqrt(|ft|). The sign of ft is ignored everywhere, so a zero ft gives +-max with the sign of fs alone.
PS2_FLOAT_NOINLINE inline float ps2_frsqrt_slow(float a, float b)
{
    if ((std::bit_cast<uint32_t>(b) & PS2_FLOAT_EXP_MASK) == 0u)
        return std::bit_cast<float>((std::bit_cast<uint32_t>(a) & PS2_FLOAT_SIGN_MASK) | PS2_FLOAT_MAX_BITS);
    // The root is rounded to a float before the division, as on the hardware.
    const float root = ps2_from_double(std::sqrt(std::fabs(ps2_to_double(b))));
    return ps2_from_double(ps2_to_double(a) / static_cast<double>(root));
}

// True when a, b and the host result r are all normal numbers, so r is also the PS2 result.
inline bool ps2_fall_normal(float a, float b, float r)
{
    // Doubling drops the sign; adding one exponent step wraps exponent 255 round to 0 next to exponent 0.
    const uint32_t sa = std::bit_cast<uint32_t>(a) * 2u + 0x01000000u;
    const uint32_t sb = std::bit_cast<uint32_t>(b) * 2u + 0x01000000u;
    const uint32_t sr = std::bit_cast<uint32_t>(r) * 2u + 0x01000000u;
    uint32_t lowest = sa < sb ? sa : sb;
    lowest = lowest < sr ? lowest : sr;
    return lowest >= 0x02000000u;
}

// The same test on the result alone. An exponent-255 operand always leaves exponent 255 in the host result
// of an addition, subtraction or multiplication (Inf or NaN), so this finds those operands as well.
inline bool ps2_fnormal(float r)
{
    return (std::bit_cast<uint32_t>(r) * 2u + 0x01000000u) >= 0x02000000u;
}

inline float ps2_fadd(float a, float b)
{
    const float r = a + b;
    return ps2_fnormal(r) ? r : ps2_fadd_slow(a, b);
}

inline float ps2_fsub(float a, float b)
{
    const float r = a - b;
    return ps2_fnormal(r) ? r : ps2_fsub_slow(a, b);
}

inline float ps2_fmul(float a, float b)
{
    const float r = a * b;
    // [gta-recomp] an exact +-0 product is the PS2's too (a zero operand, or an underflow the PS2 flushes; inf / NaN
    // never give 0): no slow path for it - multiplying by 0 is common (vector components)
    return ps2_fnormal(r) || r == 0.0f ? r : ps2_fmul_slow(a, b);
}

inline float ps2_fdiv(float a, float b)
{
    const float r = a / b;
    return ps2_fall_normal(a, b, r) ? r : ps2_fdiv_slow(a, b);
}

inline float ps2_fsqrt(float a)
{
    const float r = std::sqrt(a);
    return ps2_fall_normal(a, a, r) ? r : ps2_fsqrt_slow(a);
}

inline float ps2_frsqrt(float a, float b)
{
    const float r = a / std::sqrt(b);
    return ps2_fall_normal(a, b, r) ? r : ps2_frsqrt_slow(a, b);
}

enum class Ps2VecOp
{
    Add,
    Sub,
    Mul
};

// Lane by lane through the scalar helpers; only reached when a result lane has exponent 255.
PS2_FLOAT_NOINLINE inline __m128 ps2_vslow(__m128 a, __m128 b, Ps2VecOp op)
{
    alignas(16) float la[4], lb[4], lr[4];
    _mm_store_ps(la, a);
    _mm_store_ps(lb, b);
    for (int i = 0; i < 4; ++i)
    {
        switch (op)
        {
        case Ps2VecOp::Add:
            lr[i] = ps2_fadd_slow(la[i], lb[i]);
            break;
        case Ps2VecOp::Sub:
            lr[i] = ps2_fsub_slow(la[i], lb[i]);
            break;
        default:
            lr[i] = ps2_fmul_slow(la[i], lb[i]);
            break;
        }
    }
    return _mm_load_ps(lr);
}

// r is the host result of a op b. As in ps2_fnormal, testing r alone finds every lane that needs the double
// path. Results with exponent 0 are flushed to +-0.
inline __m128 ps2_vfinish(__m128 a, __m128 b, __m128 r, Ps2VecOp op)
{
    const __m128i expMask = _mm_set1_epi32(0x7F800000);
    const __m128i rexp = _mm_and_si128(_mm_castps_si128(r), expMask);
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(rexp, expMask)) != 0)
        return ps2_vslow(a, b, op);
    const __m128i flush = _mm_and_si128(_mm_cmpeq_epi32(rexp, _mm_setzero_si128()), _mm_set1_epi32(0x7FFFFFFF));
    return _mm_castsi128_ps(_mm_andnot_si128(flush, _mm_castps_si128(r)));
}

inline __m128 ps2_vadd(__m128 a, __m128 b)
{
    return ps2_vfinish(a, b, _mm_add_ps(a, b), Ps2VecOp::Add);
}

inline __m128 ps2_vsub(__m128 a, __m128 b)
{
    return ps2_vfinish(a, b, _mm_sub_ps(a, b), Ps2VecOp::Sub);
}

inline __m128 ps2_vmul(__m128 a, __m128 b)
{
    return ps2_vfinish(a, b, _mm_mul_ps(a, b), Ps2VecOp::Mul);
}

#define PS2_VADD(a, b) ps2_vadd((__m128)(a), (__m128)(b))
#define PS2_VSUB(a, b) ps2_vsub((__m128)(a), (__m128)(b))
#define PS2_VMUL(a, b) ps2_vmul((__m128)(a), (__m128)(b))

#define FPU_ADD_S(a, b) ps2_fadd((float)(a), (float)(b))
#define FPU_SUB_S(a, b) ps2_fsub((float)(a), (float)(b))
#define FPU_MUL_S(a, b) ps2_fmul((float)(a), (float)(b))
#define FPU_DIV_S(a, b) ps2_fdiv((float)(a), (float)(b))
#define FPU_CVT_S_W(a) ((float)(int32_t)(a))
// [gta-recomp] The EE FPU's CVT.W.S always truncates (rounds toward zero) and saturates: it has no rounding-mode
// control and no NaN / infinity (exponent 255 is just a large number). nearbyintf rounded to nearest, so every
// float -> int conversion in the game could come out one higher - VCS picked the wrong world-streaming cell
// (hex grid) at places, loading another cell's geometry and collision: the player fell through the world there.
static inline int32_t ps2FpuCvtWS(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent >= 158u) // |f| >= 2^31 (and exponent 255): saturate by sign
        return (bits & 0x80000000u) ? INT32_MIN : INT32_MAX;
    return static_cast<int32_t>(f); // C++ float -> int truncates toward zero
}
#define FPU_CVT_W_S(a) (ps2FpuCvtWS((float)(a)))
