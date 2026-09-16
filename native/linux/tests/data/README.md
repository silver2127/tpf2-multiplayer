# Geometry formatter regression

`geometry_quarter_native.txt` is the original sorted 718-edge native Linux
geometry dump from the shared test save. The source save SHA256 is
`2037d03ff4bec61843e024f3204ae55fd8c40a7c3632e471655ccc0efe2d1f2c` on both peers.
It is test data, not a replacement world or modified game script.

The four raw float32 positions in `lua_fixed_format_test.cpp` were recovered
from the decompressed save. Their exact quarter-unit heights format differently
under glibc's ties-to-even and the Windows game's legacy CRT ties-away rule.
The fixture verifies the complete observed Windows edge/height hashes after
formatting those values and verifies that an actual additional height change
still changes the hash. All other edge strings remain part of the hash.

The compatibility adapter applies only at the verified Linux build 35924 Lua
floating-format call, for single `%f`/`%F` conversions under `FE_TONEAREST`.
It corrects an exact tie's final decimal digit after glibc formats the original
double. It never changes numeric game state, Lua source, the hash algorithm,
buffer length or the floating-point environment. Other callers/formats,
unknown builds and directed rounding modes retain libc behavior.
