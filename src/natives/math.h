#pragma once

#include "../vm.h"
#include "../zym.h"

// Math functions - Basic arithmetic
ZymValue nativeMath_abs(ZymVM* vm, ZymValue value);
ZymValue nativeMath_floor(ZymVM* vm, ZymValue value);
ZymValue nativeMath_ceil(ZymVM* vm, ZymValue value);
ZymValue nativeMath_round(ZymVM* vm, ZymValue value);
ZymValue nativeMath_trunc(ZymVM* vm, ZymValue value);
ZymValue nativeMath_sign(ZymVM* vm, ZymValue value);

// Math functions - Power and exponential
ZymValue nativeMath_sqrt(ZymVM* vm, ZymValue value);
ZymValue nativeMath_cbrt(ZymVM* vm, ZymValue value);
ZymValue nativeMath_pow(ZymVM* vm, ZymValue base, ZymValue exponent);
ZymValue nativeMath_exp(ZymVM* vm, ZymValue value);
ZymValue nativeMath_exp2(ZymVM* vm, ZymValue value);
ZymValue nativeMath_expm1(ZymVM* vm, ZymValue value);

// Math functions - Logarithmic
ZymValue nativeMath_log(ZymVM* vm, ZymValue value);
ZymValue nativeMath_log10(ZymVM* vm, ZymValue value);
ZymValue nativeMath_log2(ZymVM* vm, ZymValue value);
ZymValue nativeMath_log1p(ZymVM* vm, ZymValue value);

// Math functions - Trigonometric
ZymValue nativeMath_sin(ZymVM* vm, ZymValue value);
ZymValue nativeMath_cos(ZymVM* vm, ZymValue value);
ZymValue nativeMath_tan(ZymVM* vm, ZymValue value);
ZymValue nativeMath_asin(ZymVM* vm, ZymValue value);
ZymValue nativeMath_acos(ZymVM* vm, ZymValue value);
ZymValue nativeMath_atan(ZymVM* vm, ZymValue value);
ZymValue nativeMath_atan2(ZymVM* vm, ZymValue y, ZymValue x);

// Math functions - Hyperbolic
ZymValue nativeMath_sinh(ZymVM* vm, ZymValue value);
ZymValue nativeMath_cosh(ZymVM* vm, ZymValue value);
ZymValue nativeMath_tanh(ZymVM* vm, ZymValue value);
ZymValue nativeMath_asinh(ZymVM* vm, ZymValue value);
ZymValue nativeMath_acosh(ZymVM* vm, ZymValue value);
ZymValue nativeMath_atanh(ZymVM* vm, ZymValue value);

// Math functions - Min/Max
ZymValue nativeMath_min(ZymVM* vm, ZymValue a, ZymValue b);
ZymValue nativeMath_max(ZymVM* vm, ZymValue a, ZymValue b);
ZymValue nativeMath_clamp(ZymVM* vm, ZymValue value, ZymValue minVal, ZymValue maxVal);

// Math functions - Modulo and remainder
ZymValue nativeMath_fmod(ZymVM* vm, ZymValue x, ZymValue y);
ZymValue nativeMath_remainder(ZymVM* vm, ZymValue x, ZymValue y);

// Math functions - Other
ZymValue nativeMath_hypot(ZymVM* vm, ZymValue x, ZymValue y);
ZymValue nativeMath_toRadians(ZymVM* vm, ZymValue degrees);
ZymValue nativeMath_toDegrees(ZymVM* vm, ZymValue radians);
ZymValue nativeMath_isNaN(ZymVM* vm, ZymValue value);
ZymValue nativeMath_isInfinite(ZymVM* vm, ZymValue value);
ZymValue nativeMath_isFinite(ZymVM* vm, ZymValue value);
ZymValue nativeMath_lerp(ZymVM* vm, ZymValue start, ZymValue end, ZymValue t);
ZymValue nativeMath_map(ZymVM* vm, ZymValue value, ZymValue inMin, ZymValue inMax, ZymValue outMin, ZymValue outMax);

// Register math natives into the VM
void registerMathNatives(VM* vm);
