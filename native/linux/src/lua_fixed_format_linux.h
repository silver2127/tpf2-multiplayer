#pragma once

// Lua 5.2 emits one printf conversion at a time. Recognize only its fixed
// decimal grammar; every other conversion must go directly to libc.
bool Tpf2mpLuaFixedPrecision(const char* format, int* precision);

// Correct glibc's nearest-even result only at an EXACT halfway boundary where
// the legacy Windows CRT chooses the next decimal digit away from zero. The
// output length never changes. The source number and fenv remain untouched.
// Directed floating-point rounding modes retain libc's behavior.
bool Tpf2mpWindowsFixedTieCorrection(char* output, int length, double value, int precision);
