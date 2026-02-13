#include <stdio.h>
#include <math.h>
#include "math.h"

// =============================================================================
// MATH FUNCTIONS
// =============================================================================

// Basic arithmetic functions
ZymValue nativeMath_abs(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "abs() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(fabs(zym_asNumber(value)));
}

ZymValue nativeMath_floor(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "floor() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(floor(zym_asNumber(value)));
}

ZymValue nativeMath_ceil(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "ceil() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(ceil(zym_asNumber(value)));
}

ZymValue nativeMath_round(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "round() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(round(zym_asNumber(value)));
}

ZymValue nativeMath_trunc(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "trunc() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(trunc(zym_asNumber(value)));
}

ZymValue nativeMath_sign(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "sign() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num > 0) return zym_newNumber(1.0);
    if (num < 0) return zym_newNumber(-1.0);
    return zym_newNumber(0.0);
}

// Power and exponential functions
ZymValue nativeMath_sqrt(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "sqrt() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num < 0) {
        zym_runtimeError(vm, "sqrt() requires a non-negative argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(sqrt(num));
}

ZymValue nativeMath_cbrt(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "cbrt() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(cbrt(zym_asNumber(value)));
}

ZymValue nativeMath_pow(ZymVM* vm, ZymValue base, ZymValue exponent) {
    if (!zym_isNumber(base) || !zym_isNumber(exponent)) {
        zym_runtimeError(vm, "pow() requires two number arguments");
        return ZYM_ERROR;
    }
    return zym_newNumber(pow(zym_asNumber(base), zym_asNumber(exponent)));
}

ZymValue nativeMath_exp(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "exp() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(exp(zym_asNumber(value)));
}

ZymValue nativeMath_exp2(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "exp2() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(exp2(zym_asNumber(value)));
}

ZymValue nativeMath_expm1(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "expm1() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(expm1(zym_asNumber(value)));
}

// Logarithmic functions
ZymValue nativeMath_log(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "log() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num <= 0) {
        zym_runtimeError(vm, "log() requires a positive argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(log(num));
}

ZymValue nativeMath_log10(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "log10() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num <= 0) {
        zym_runtimeError(vm, "log10() requires a positive argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(log10(num));
}

ZymValue nativeMath_log2(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "log2() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num <= 0) {
        zym_runtimeError(vm, "log2() requires a positive argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(log2(num));
}

ZymValue nativeMath_log1p(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "log1p() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num <= -1) {
        zym_runtimeError(vm, "log1p() requires an argument greater than -1");
        return ZYM_ERROR;
    }
    return zym_newNumber(log1p(num));
}

// Trigonometric functions
ZymValue nativeMath_sin(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "sin() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(sin(zym_asNumber(value)));
}

ZymValue nativeMath_cos(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "cos() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(cos(zym_asNumber(value)));
}

ZymValue nativeMath_tan(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "tan() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(tan(zym_asNumber(value)));
}

ZymValue nativeMath_asin(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "asin() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num < -1.0 || num > 1.0) {
        zym_runtimeError(vm, "asin() requires an argument in the range [-1, 1]");
        return ZYM_ERROR;
    }
    return zym_newNumber(asin(num));
}

ZymValue nativeMath_acos(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "acos() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num < -1.0 || num > 1.0) {
        zym_runtimeError(vm, "acos() requires an argument in the range [-1, 1]");
        return ZYM_ERROR;
    }
    return zym_newNumber(acos(num));
}

ZymValue nativeMath_atan(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "atan() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(atan(zym_asNumber(value)));
}

ZymValue nativeMath_atan2(ZymVM* vm, ZymValue y, ZymValue x) {
    if (!zym_isNumber(y) || !zym_isNumber(x)) {
        zym_runtimeError(vm, "atan2() requires two number arguments");
        return ZYM_ERROR;
    }
    return zym_newNumber(atan2(zym_asNumber(y), zym_asNumber(x)));
}

// Hyperbolic functions
ZymValue nativeMath_sinh(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "sinh() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(sinh(zym_asNumber(value)));
}

ZymValue nativeMath_cosh(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "cosh() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(cosh(zym_asNumber(value)));
}

ZymValue nativeMath_tanh(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "tanh() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(tanh(zym_asNumber(value)));
}

ZymValue nativeMath_asinh(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "asinh() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(asinh(zym_asNumber(value)));
}

ZymValue nativeMath_acosh(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "acosh() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num < 1.0) {
        zym_runtimeError(vm, "acosh() requires an argument >= 1");
        return ZYM_ERROR;
    }
    return zym_newNumber(acosh(num));
}

ZymValue nativeMath_atanh(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        zym_runtimeError(vm, "atanh() requires a number argument");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    if (num <= -1.0 || num >= 1.0) {
        zym_runtimeError(vm, "atanh() requires an argument in the range (-1, 1)");
        return ZYM_ERROR;
    }
    return zym_newNumber(atanh(num));
}

// Min/Max functions
ZymValue nativeMath_min(ZymVM* vm, ZymValue a, ZymValue b) {
    if (!zym_isNumber(a) || !zym_isNumber(b)) {
        zym_runtimeError(vm, "min() requires two number arguments");
        return ZYM_ERROR;
    }
    double numA = zym_asNumber(a);
    double numB = zym_asNumber(b);
    return zym_newNumber(numA < numB ? numA : numB);
}

ZymValue nativeMath_max(ZymVM* vm, ZymValue a, ZymValue b) {
    if (!zym_isNumber(a) || !zym_isNumber(b)) {
        zym_runtimeError(vm, "max() requires two number arguments");
        return ZYM_ERROR;
    }
    double numA = zym_asNumber(a);
    double numB = zym_asNumber(b);
    return zym_newNumber(numA > numB ? numA : numB);
}

// Clamping function
ZymValue nativeMath_clamp(ZymVM* vm, ZymValue value, ZymValue minVal, ZymValue maxVal) {
    if (!zym_isNumber(value) || !zym_isNumber(minVal) || !zym_isNumber(maxVal)) {
        zym_runtimeError(vm, "clamp() requires three number arguments");
        return ZYM_ERROR;
    }
    double num = zym_asNumber(value);
    double min = zym_asNumber(minVal);
    double max = zym_asNumber(maxVal);

    if (min > max) {
        zym_runtimeError(vm, "clamp() requires min <= max");
        return ZYM_ERROR;
    }

    if (num < min) return zym_newNumber(min);
    if (num > max) return zym_newNumber(max);
    return zym_newNumber(num);
}

// Modulo and remainder
ZymValue nativeMath_fmod(ZymVM* vm, ZymValue x, ZymValue y) {
    if (!zym_isNumber(x) || !zym_isNumber(y)) {
        zym_runtimeError(vm, "fmod() requires two number arguments");
        return ZYM_ERROR;
    }
    double divisor = zym_asNumber(y);
    if (divisor == 0.0) {
        zym_runtimeError(vm, "fmod() division by zero");
        return ZYM_ERROR;
    }
    return zym_newNumber(fmod(zym_asNumber(x), divisor));
}

ZymValue nativeMath_remainder(ZymVM* vm, ZymValue x, ZymValue y) {
    if (!zym_isNumber(x) || !zym_isNumber(y)) {
        zym_runtimeError(vm, "remainder() requires two number arguments");
        return ZYM_ERROR;
    }
    double divisor = zym_asNumber(y);
    if (divisor == 0.0) {
        zym_runtimeError(vm, "remainder() division by zero");
        return ZYM_ERROR;
    }
    return zym_newNumber(remainder(zym_asNumber(x), divisor));
}

// Hypotenuse
ZymValue nativeMath_hypot(ZymVM* vm, ZymValue x, ZymValue y) {
    if (!zym_isNumber(x) || !zym_isNumber(y)) {
        zym_runtimeError(vm, "hypot() requires two number arguments");
        return ZYM_ERROR;
    }
    return zym_newNumber(hypot(zym_asNumber(x), zym_asNumber(y)));
}

// Conversion functions (degrees/radians)
ZymValue nativeMath_toRadians(ZymVM* vm, ZymValue degrees) {
    if (!zym_isNumber(degrees)) {
        zym_runtimeError(vm, "toRadians() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(zym_asNumber(degrees) * M_PI / 180.0);
}

ZymValue nativeMath_toDegrees(ZymVM* vm, ZymValue radians) {
    if (!zym_isNumber(radians)) {
        zym_runtimeError(vm, "toDegrees() requires a number argument");
        return ZYM_ERROR;
    }
    return zym_newNumber(zym_asNumber(radians) * 180.0 / M_PI);
}

// Classification functions
ZymValue nativeMath_isNaN(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        return zym_newBool(false);
    }
    return zym_newBool(isnan(zym_asNumber(value)));
}

ZymValue nativeMath_isInfinite(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        return zym_newBool(false);
    }
    return zym_newBool(isinf(zym_asNumber(value)));
}

ZymValue nativeMath_isFinite(ZymVM* vm, ZymValue value) {
    if (!zym_isNumber(value)) {
        return zym_newBool(false);
    }
    return zym_newBool(isfinite(zym_asNumber(value)));
}

// Linear interpolation
ZymValue nativeMath_lerp(ZymVM* vm, ZymValue start, ZymValue end, ZymValue t) {
    if (!zym_isNumber(start) || !zym_isNumber(end) || !zym_isNumber(t)) {
        zym_runtimeError(vm, "lerp() requires three number arguments");
        return ZYM_ERROR;
    }
    double a = zym_asNumber(start);
    double b = zym_asNumber(end);
    double factor = zym_asNumber(t);
    return zym_newNumber(a + factor * (b - a));
}

// Map value from one range to another
ZymValue nativeMath_map(ZymVM* vm, ZymValue value, ZymValue inMin, ZymValue inMax, ZymValue outMin, ZymValue outMax) {
    if (!zym_isNumber(value) || !zym_isNumber(inMin) || !zym_isNumber(inMax) ||
        !zym_isNumber(outMin) || !zym_isNumber(outMax)) {
        zym_runtimeError(vm, "map() requires five number arguments");
        return ZYM_ERROR;
    }

    double val = zym_asNumber(value);
    double in_min = zym_asNumber(inMin);
    double in_max = zym_asNumber(inMax);
    double out_min = zym_asNumber(outMin);
    double out_max = zym_asNumber(outMax);

    if (in_min == in_max) {
        zym_runtimeError(vm, "map() requires inMin != inMax");
        return ZYM_ERROR;
    }

    return zym_newNumber((val - in_min) * (out_max - out_min) / (in_max - in_min) + out_min);
}

// =============================================================================
// Registration
// =============================================================================

void registerMathNatives(VM* vm) {
    // Basic arithmetic
    zym_defineNative(vm, "abs(value)", nativeMath_abs);
    zym_defineNative(vm, "floor(value)", nativeMath_floor);
    zym_defineNative(vm, "ceil(value)", nativeMath_ceil);
    zym_defineNative(vm, "round(value)", nativeMath_round);
    zym_defineNative(vm, "trunc(value)", nativeMath_trunc);
    zym_defineNative(vm, "sign(value)", nativeMath_sign);

    // Power and exponential
    zym_defineNative(vm, "sqrt(value)", nativeMath_sqrt);
    zym_defineNative(vm, "cbrt(value)", nativeMath_cbrt);
    zym_defineNative(vm, "pow(base, exponent)", nativeMath_pow);
    zym_defineNative(vm, "exp(value)", nativeMath_exp);
    zym_defineNative(vm, "exp2(value)", nativeMath_exp2);
    zym_defineNative(vm, "expm1(value)", nativeMath_expm1);

    // Logarithmic
    zym_defineNative(vm, "log(value)", nativeMath_log);
    zym_defineNative(vm, "log10(value)", nativeMath_log10);
    zym_defineNative(vm, "log2(value)", nativeMath_log2);
    zym_defineNative(vm, "log1p(value)", nativeMath_log1p);

    // Trigonometric
    zym_defineNative(vm, "sin(value)", nativeMath_sin);
    zym_defineNative(vm, "cos(value)", nativeMath_cos);
    zym_defineNative(vm, "tan(value)", nativeMath_tan);
    zym_defineNative(vm, "asin(value)", nativeMath_asin);
    zym_defineNative(vm, "acos(value)", nativeMath_acos);
    zym_defineNative(vm, "atan(value)", nativeMath_atan);
    zym_defineNative(vm, "atan2(y, x)", nativeMath_atan2);

    // Hyperbolic
    zym_defineNative(vm, "sinh(value)", nativeMath_sinh);
    zym_defineNative(vm, "cosh(value)", nativeMath_cosh);
    zym_defineNative(vm, "tanh(value)", nativeMath_tanh);
    zym_defineNative(vm, "asinh(value)", nativeMath_asinh);
    zym_defineNative(vm, "acosh(value)", nativeMath_acosh);
    zym_defineNative(vm, "atanh(value)", nativeMath_atanh);

    // Min/Max
    zym_defineNative(vm, "min(a, b)", nativeMath_min);
    zym_defineNative(vm, "max(a, b)", nativeMath_max);
    zym_defineNative(vm, "clamp(value, minVal, maxVal)", nativeMath_clamp);

    // Modulo and remainder
    zym_defineNative(vm, "fmod(x, y)", nativeMath_fmod);
    zym_defineNative(vm, "remainder(x, y)", nativeMath_remainder);

    // Other
    zym_defineNative(vm, "hypot(x, y)", nativeMath_hypot);
    zym_defineNative(vm, "toRadians(degrees)", nativeMath_toRadians);
    zym_defineNative(vm, "toDegrees(radians)", nativeMath_toDegrees);
    zym_defineNative(vm, "isNaN(value)", nativeMath_isNaN);
    zym_defineNative(vm, "isInfinite(value)", nativeMath_isInfinite);
    zym_defineNative(vm, "isFinite(value)", nativeMath_isFinite);
    zym_defineNative(vm, "lerp(start, end, t)", nativeMath_lerp);
    zym_defineNative(vm, "map(value, inMin, inMax, outMin, outMax)", nativeMath_map);
}
