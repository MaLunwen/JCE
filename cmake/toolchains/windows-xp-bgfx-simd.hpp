#pragma once

// BX selects an SSE4.1 implementation from __SSE2__. Use its existing
// reference branch while keeping SSE2 available to the compiler.
#if !defined(__SSE2__) || defined(__SSE3__) || defined(__SSSE3__) || \
    defined(__SSE4_1__) || defined(__SSE4_2__) || defined(__SSE4A__) || \
    defined(__POPCNT__) || defined(__AVX__) || defined(__FMA__) || \
    defined(__BMI__) || defined(__BMI2__) || defined(__LZCNT__) || \
    defined(__AES__) || defined(__PCLMUL__)
#error The XP BGFX provider requires an SSE2 compiler target
#endif

// Leave BX's include order intact so BGFX and bimg can install debug handlers.
#undef __SSE2__
