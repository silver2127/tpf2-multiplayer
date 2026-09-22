# dev 8e31f1e0: label, station and window tint diagnostics

Static investigation of the supplied native Steam build 35924; `readelf -n`
confirmed build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
The executable was never run. Addresses below are ELF virtual addresses,
analysis evidence only, not new patch sites. Continues
[db8a4776](DEV_DB8A4776.md) and [59bb258a](DEV_59BB258A.md).

## Incoming behavior

Windows `8e31f1e01a0275c4fe8b39c65a56a362af4c45c7` introduces a counted
StationLabelTint wrapper around IconTintForEntity, logs its first six results
and periodic labels/tinted totals. WindowTint logs its first six calls before
engine/window/ownership checks. StationIconTint logs its first six no-owner
failures across two stages: no StationGroup slot, or first station lacking
PlayerOwned. This changes diagnostics, not the underlying ownership policy.
Linux currently implements none of these tint helpers.

## Fresh window bind trace

Searched functions/signature exports and disassembled the complete
0x1444ee0 binding helper, then followed 0x3129f10 into 0x30518e0. The
ViewCreator source/string anchors are documented in [d3135a59](DEV_D3135A59.md).
SysV arguments at the bind entry are rdi=widget and esi=entity value:
0x1444ee1 (`41 89 f0`) moves esi into r8d for integer formatting;
0x1444ef5 (`49 89 fd`) preserves the widget in r13. Before the call at
0x1444f85 (`e8 86 4f ce 01`), rsi points to the temporary libstdc++ string
and rdi is restored from r13. The temporary is freed after the call.

0x3129f20 (`e8 bb 79 f2 ff`) calls 0x30518e0 with those arguments.
The latter tests widget+0x48, then at 0x30518f8 (`48 8d 7b 40`) selects
widget+0x40 as the destination of std::string::_M_assign at 0x30518ff
(`e8 dc 72 98 fd`). Nonempty old/new values go through 0x307e6f0 and
0x307e430 with the widget and its +0x18 field. Together with the
`temp.view.entity_` prefix this supports an ID setter/registration path,
not a style-vector append. The observed +0x40 string must not be treated
as the Windows class list. The complete bind includes exception landing
pads at 0x1444fe0 and 0x1444fec; an entry trampoline would require proven
relocation, unwind and register preservation. No diagnostic-only detour
was installed: it would not observe WindowTint or its ownership decisions.

## Label and no-owner follow-up

Rechecked the label colour selection and draw call directly in the ELF:
0x138952e (`48 0f 44 f1`) chooses a stack colour; 0x1389549
(`48 89 f2`) supplies rdx; 0x1389559 (`e8 12 a0 ff ff`) calls
0x1383570. The following click registration loads its entity pointer from
rbp-0xd0 at 0x1389565. functions.csv places this in the 2440-byte body
starting at 0x1389180. This confirms the previous candidate, but not a
safe complete engine/entity/owner path or relay. The earlier aligned
colour-pointer constraint still applies. Counting arbitrary draw calls
would not implement the new labels/tinted distinction.

Revisited the HUD aggregate helper 0x1092ca0. It saves its context from rsi
in r12 and calls 0x146f0a0 to obtain the engine. The first type comes from
context+0xc, entity pointer from context+8, before 0x1092cdf calls the
asserting 0x9e5590 slot accessor. The second type comes from context+0x10,
with rsi pointing at the first component, before the call at 0x1092d39.
The paged strides are 24 and 264 bytes (0x1092d15..0x1092d1c and
0x1092d6f..0x1092d7c). These accesses do not prove a StationGroup vector
or PlayerOwned fallback. In particular, substituting this asserting path
for a missing-component probe would reintroduce the failure Windows fixed.

## Decision / missing proof

All three diagnostic additions remain unported. Needed evidence remains:
label entity/engine lifetime and aligned tint relay; safe non-asserting
StationGroup storage/first-station owner traversal with HUD entity flow;
window style mutation and owner lookup across world changes; and safe
native detours with complete guard/unwind contracts. The window trace
narrows the ID setter, but does not close these prerequisites. Native tint
stays unavailable; existing guarded visibility and permission patches are
unchanged. No speculative address, byte patch or struct offset was added.
