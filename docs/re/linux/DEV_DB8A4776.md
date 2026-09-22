# dev db8a4776: safe station tint, button root and class diagnostics

Read-only static investigation of the supplied Linux Steam build 35924.
`readelf -n` confirms build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`.
Addresses below are ELF virtual addresses, not installed patch sites. The
binary was never executed. This continues [5fb7aea2](DEV_5FB7AEA2.md).

## Windows change

`db8a477649d9ed1000023eed4d5cb4afa290d9d3` replaces the asserting
StationGroup accessor with TrainOrderSlot plus a stride-parameterized pool
read. It moves the hook from content creation to the ItemButton root after
RVA 0x5e38dc's call (hook 0x5e38e1, `48 8b f8 45 33 e4`). It adds a
`tintclass=mpCo` override, first-four class-list read-backs and periodic
window/station counters. Windows component+0xb0/+0xb8 and MSVC strings are
not native layout evidence. Both incoming files remain byte-exact upstream.

## Fresh wrapper investigation

Disassembled Linux 0x1095937..0x1095a3e and the complete wrapper at
0x306fd80..0x306fe43, following the previous StationItem string anchor.
At 0x1095972, `e8 d9 a8 ff ff` builds station content with 0x1090250.
At 0x1095991, `bf 40 04 00 00` requests a 0x440-byte allocation;
0x1095996 calls operator new. At 0x109599b the content is loaded into rsi;
rdx=r14 is prepared at 0x10959a2; rdi=allocation at 0x10959a5; and
`49 89 c5` at 0x10959a8 keeps that allocation in r13. The call at
0x10959ab (`e8 d0 a3 fd 01`) targets 0x306fd80.

The wrapper moves a libstdc++ string from rdx into a stack temporary:
0x306fd93 reads [rdx], 0x306fdad computes rdx+0x10, and 0x306fdb1
compares them to distinguish inline storage. Length comes from [rdx+8]
at 0x306fdc2. The original string is reset at 0x306fdc6..0x306fdd1.
It passes a pointer to a content-pointer temporary in rsi to 0x306faf0
at 0x306fde4, then destroys any remaining content through vtable+8 at
0x306fdf5 and frees the temporary string. Crucially, 0x306fe0a loads the
stack canary into rax, and 0x306fe0e XORs it with fs:0x28 before return.
**rax is not the button pointer.** A native post-wrapper relay would need
the preserved r13 allocation and a proven entity/engine path, not Windows'
rax/ebx recipe. This does not yet prove a safe depot path, all live state,
or an insertion span free from interior branch targets.

## Missing-component assertion

`funcsig.csv` identifies 0x9e5590 as
`ecs::Engine::GetComponentDataIndex(const ecs::Entity&, int) const`.
Fresh disassembly confirms SysV rdi=engine, rsi=entity pointer, edx=type.
At 0x9e55bd it reads the entity; 0x9e55c4 (`48 8b 87 98 00 00 00`)
loads engine+0x98, then indexes 24-byte entity records. The scan compares
8-byte type/slot pairs. The miss path reaches 0x9e570f (`ba 23 01 00 00`,
line 291), then 0x9e571b loads 0x3f217f3, whose actual ELF bytes spell
`it != components.end()`. At 0x9e5722 it calls 0x2fcb5e0. This accessor
must not be used to probe towns/industries for StationGroup.

Existing Linux NameComponent already scans without asserting, using native
entity and pool layouts; it has no station tint caller. Its Name stride and
pool layout are not sufficient proof of StationGroup storage or the live
UI-engine lifetime. No replacement asserting accessor was introduced.

## Style read-back attempt

Searched all addStyleClass xrefs and followed the third binding path,
0x1ce37d0..0x1ce3c17, which the previous integration had not resolved.
At 0x1ce3bce (`48 8d 35 ee d9 25 02`) it loads the method-name string
at 0x3f415c3. It constructs a temporary through 0x1c974e0 at 0x1ce3bd8,
looks up a destination through 0xc44a30 at 0x1ce3be3 and move-assigns a
string via 0x9b7e90 at 0x1ce3bf2. This is another metadata/string path,
not a proven CComponent style mutation or class-list accessor. It supplies
no evidence for using Windows' component+0xb0 class-vector offset.

## Decision / missing evidence

Keep native station/window tint unavailable. Missing are the complete
station/depot entity and live-engine flow, StationGroup type/pool and
first-station owner contract, native style append and class-list layout,
and exception/lifetime-safe relays. Consequently the button-root correction,
non-asserting station lookup, tint-class override, read-back and counters
cannot be enabled independently of the absent tint implementation. Existing
visibility/permission patches remain intact and byte-verified. No guessed
address, offset, Windows ABI or ineffective configuration switch was added.
