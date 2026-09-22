# Native vehicle and station-label colors

Steam Linux build 35924, build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
This extends the earlier static investigations in DEV_D3135A59 and DEV_59BB258A.

## Vehicle draw

The four calls at 0x138bcd8, 0x138c08d, 0x138c359 and 0x138c4dd target
0x1385ec0. All retain ItemCreatorImpl in rbx and entity* in r12, used by
subsequent click registration at 0x1381a70. Engine is impl+0x28, as established
by the owner lookup at 0x138ba89. The relay adds these as SysV integer arguments;
the four original geometry floats remain in xmm0..3. A two-qword return retains
the exact original rectangle in rax/rdx.

0x137cce0 selects the ordinary vertex batch by layer and texture pointer.
0x137d010 selects the colored batch using the same keys, from a separate inner
map at layer-node+0x60 instead of +0x30. Their resource names are respectively
`HudVertexBuffer` (0x3f3244b) and `HudColorVertexBuffer` (0x3f3245b).
Batch+8/+16/+24 is the vertex vector's begin/end/capacity. The former's stride
is 16 bytes (position, UV); the latter's is 32 bytes (position, UV, RGBA).

The wrapper calls the original draw, checks that exactly six vertices were
appended, copies those exact positions and UVs into colored vertices, and uses
the game's colored batch and allocator. 0x1385be0 grows that vector; its
0x1385c52..0x1385c70 instructions copy both 16-byte halves before returning.
Only after the colored append completes are the six plain vertices removed.
No heap allocation is transferred between C++ runtimes. Geometry, texture,
layer and click rectangle are retained; no simulation state changes.

## Station labels

0x1389559 calls 0x1383570 with buffer, texture, required aligned RGBA pointer,
and two packed geometry values. The next click registration obtains entity*
from rbp-0xd0. The relay passes that pointer as the sixth SysV argument. The
wrapper obtains the current render engine through CGameUI+0x448, CGame+0x150,
and state+0x28. It uses the bounded, non-asserting ECS owner lookup. A 16-byte
aligned thread-local color satisfies the callee's `movdqa` at 0x13836bf.
The callee copies RGBA into all six 32-byte vertices, including its grow path.

Both paths use the same 20 RGB colors and golden-angle extension as Windows,
including one's own company; missing owners/mappings retain the original draw.
`iconcolor=0` disables both. `company_draw_checks_linux.h` records the byte
checks for the calls, context, vector layouts and copy paths before patching.

## Evidence

The soldier SDK CTest executes the real wrappers against a synthetic ECS and
fake native draw/buffer/grow functions. It checks the returned rectangle,
original positions/UVs, layer/texture, colored growth, unowned fallback, and
label alignment/current-world lookup. All 51 tests pass. Live visual testing
is still pending; successful fixture tests do not establish rendering fidelity.
