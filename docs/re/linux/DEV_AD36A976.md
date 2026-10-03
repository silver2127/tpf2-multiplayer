# dev ad36a976: cargo-filter replay writer investigation

Windows target `ad36a9769fc4059c58ee7554e5852497bff99df1` replaces the
ineffective Lua stopConfig setters with a request file consumed synchronously
by the Windows UpdateLine factory hook. Windows uses its vector<uint32>
copy constructor at RVA 0x125480 for both packed 32-bit words and float bits.
The upstream live Windows measurement is not a Linux observation.

## Static evidence

Read the actual lab ELF, Steam build 35924, GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a`, using readelf and objdump.
Revisited [the cargo capture investigation](DEV_F9D34252.md),
[the line ABI](SLICE_LINES.md), and Linux functions/funcsig exports.
All addresses below are PIE RVAs. No new patch or native memory writer ships.

- `0x228b377` writes the StopConfig member offset 0x50
  (`48 c7 85 90 fc ff ff 50 00 00 00`); the binding call at
  `0x228b414` targets `0x2285020`. The maxLoad registration writes
  offset 0x50 within StopConfig at `0x228b5b0`, then calls
  `0x2285690` at `0x228b5cf`. Exported StopConfig signatures identify
  load/unload vector<int> getter/setter properties and the float member.
- The exported FDE range `0xb83190`, size 1391, is a stop range copy:
  rdi=source begin, rsi=source end, rdx=destination. It moves these to
  r12, [rbp-0x38], rbx and advances source/destination by 0xb8 at
  `0xb83525/0xb8352c`. The bit-container bodies are inlined here;
  they are not independent function entries suitable for direct calls.
- Load starts at Stop+0x50: word pointers +0x50/+0x60, 32-bit bit
  positions +0x58/+0x68, capacity end +0x70. Unload starts +0x78:
  pointers +0x78/+0x88, positions +0x80/+0x90, capacity end +0x98.
  The calculation at `0xb83363..0xb83376` is
  `(endWord-beginWord)*8 + endBit-beginBit`. Tail loops compare bit
  position to 63 (`83 f9 3f` at `0xb833fc` and `0xb834fc`) and advance
  word pointers by 8. Allocation at `0xb835c0..0xb835d2` rounds by 63,
  shifts right 6 and left 3, then calls operator new at PLT `0x6dbce0`.
  This confirms 40-byte libstdc++ bit vectors with 64-bit words.
- MaxLoad is at Stop+0xa0. `0xb83511` loads source+0xa0 into rsi;
  `0xb83519` loads destination+0xa0 into rdi; `0xb83520`
  (`e8 bb 5e f5 ff`) calls `0xad93e0`. Disassembling that entire
  function confirms a three-pointer vector copy, SysV rdi=destination,
  rsi=source: subtract begin from end, divide by four at `0xad9410`,
  allocate at `0xad9430` (`e8 ab 28 c0 ff` to operator new), copy
  via memmove at `0xad9462`, and store end at `0xad946d`.
  This supplies a native float-copy candidate, not a 40-byte bool copier.
- Destructor `0xaa43f0` calls operator delete at PLT `0x6dbcd0` for
  +0xa0, +0x78, +0x50, +0x38 and +0x10; `0xaa4459`
  (`48 81 c3 b8 00 00 00`) advances the stop stride 0xb8.
- UpdateLine `0x15f0050` begins with existing verified bytes
  `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55`.
  The established SysV arguments are rdi=return slot, rsi=engine,
  edx=line entity, rcx=Line. At `0x15f0080` (`48 8b 01`) it takes
  the stop-vector begin, and at `0x15f0083` clears the source. End and
  capacity are similarly moved at `0x15f00aa..0x15f00cc`.
  The existing Linux script-factory branch only calls assignment::Replay.
  A cargo writer would have to finish before the original factory moves
  that Line, and use Linux containers and game allocation/cleanup.

Raw evidence: job `meta/live/elf-notes.txt`, `stopconfig-registration.txt`,
`stop-signatures.txt`, `stop-copy.txt`, `float-copy.txt`,
`stop-destroy.txt`, and `update-factory.txt`. The objdump nearest-export
labels are not semantic identifications of these internal functions.

## Live attempt and decision

Installed this job's five soldier-built libraries and merged mod into native
actor copies after cp -a backups with suffix `.before-port-ad36a976`.
Ran only `tools/sandbox/tpf2mp-lab run native --root ~/.local/share/tpf2mp-lab`,
under a 180-second timeout. It exited 1 immediately with
`bwrap: setting up uid map: Permission denied`. There was no game process
for gdb attachment, so no register/memory transcript can be supplied.
No menu, loaded save, Vulkan device, cargo UI, or replay was observed.
No launcher bypass or host policy change was attempted.

`meta/live/attempt.log` records the failure; `installed.json` records the
built library hashes. Both actor payloads were restored in a finally block;
`restoration.json` reports matching content manifests. Actor logs and data
were archived, but they predate this failed launch and prove no new gameplay.
No game was left running and no save was changed.

Native cargo replay writing remains **unported**. Still needed: inspect a
Lua-built Line at the script factory before/after assignment; establish that
the target containers are empty, writable and owned by this command; verify
allocation and cleanup through normal command completion and world changes;
and differentially read back filters including empty/all-zero, 64-bit word
boundaries and fractional maxLoad. The static allocator and layout evidence
alone does not meet the requested live ownership proof. Preserve the Windows
implementation and shared request protocol; do not guess a native writer.
The inherited native cargo-capture gap also remains.
