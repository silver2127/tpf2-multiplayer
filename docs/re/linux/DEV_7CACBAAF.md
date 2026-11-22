# Family range validation cost — dev 7cacbaaf

No new engine hook, ABI or layout is introduced. The actual lab ELF build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a` and all existing family/canon sites
were rechecked using `tools/linux/verify_order_canon_elf.py`.
[DEV_0115785C](DEV_0115785C.md) remains the source for their disassembly;
its snapshot implementation description is superseded here. This work does
not close that record's pre-existing engine lifetime validation gaps.

## Kernel contract

The installed `/usr/include/linux/fs.h` defines `PROCMAP_QUERY` as
`_IOWR('f', 17, struct procmap_query)`. Its fixed-width 104-byte structure is
reproduced locally because soldier's older headers lack it: nine 64-bit fields
(size, flags, address, VMA start/end/flags/page-size/offset/inode), four 32-bit
fields (device major/minor, name size, build-id size), then two 64-bit pointers.
No names or build IDs are requested. Flags 1 and 3 filter readable and
readable+writable mappings. Without COVERING_OR_NEXT, holes and incompatible
permissions return ENOENT. A zero-address initial query returning ENOENT also
proves support; unsupported/denied queries select the compatibility path.

One ioctl normally covers a range within a VMA. Multi-VMA ranges advance to
returned VMA ends and fail at any hole or protection mismatch. Cost is
proportional to intersected VMAs and kernel tree lookup, not a full mapping
text enumeration; it is not literally O(1) for arbitrary fragmented ranges.
All query errors fail closed. There is no permission cache to become stale.
As before, validating a range does not pin its mappings or prove object lifetime:
the existing simulation-boundary ownership requirement remains.

Fallback snapshots are refreshed per family walk. The scanner retains only
80 prefix bytes while discarding arbitrary-length pathnames, reads 16 KiB
chunks, rejects malformed/overflowing address fields and I/O errors, and uses
binary search to find the first intersecting readable mapping. It preserves
writability and contiguous-range checks. Unlike the modern ioctl path, it
still scales with the process's mapping count and retains vector allocations.

## Evidence and limits

The final soldier-built `test_order_canon` reported `family mapping query:
PROCMAP_QUERY` on kernel 7.0.0-31-generic. Both this path and an explicitly
forced fallback passed live kernel mmap/mprotect/munmap/remap tests, including
a three-page range whose endpoints stay readable while its middle becomes
read-only, inaccessible, unmapped, then readable/writable again. Tests retained
all seven shim register/rollback checks and all five family node widths.

A separate host fixture made 50,000 alternating-permission page VMAs and
repeated refresh+range validation. Buffered snapshots averaged 5415.95 us;
ioctl averaged 0.102 us. The benchmark source and output are retained in
`meta/live/`. This does not measure in-game throughput, frame preparation or
lavapipe behavior.

The lab game launch failed at bwrap UID-map setup before exec; no live engine
registers or GPU logs were obtained. None are needed to derive a new game
contract here, because no game address or ownership rule changed. See the
[integration record](../../linux/UPSTREAM_dev_7cacbaaf.md) for restoration and
verification results.
