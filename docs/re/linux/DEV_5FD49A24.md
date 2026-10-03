# Native terrain lookup cursor: dev 5fd49a24

The change touches mod-owned TLS and an atomic generation only. The existing
[sidecar ABI investigation](DEV_2B4FD093.md) applies unchanged. The generation
is advanced at native LoadHook entry, before calling the engine, covering both
local-file and streamed sidecars. It is a hint invalidator, not a terrain
ownership or publication primitive. No new ELF address or field is introduced.

Fresh read-only verification of the lab build confirms GNU build-id
`3a0e156390b0e6f1e372051c24802c8493ae454a` and all 39 sites in
`bigmap/linux/sites.json`. Fresh objdump at `0xcf73f2..0xcf741d` confirms:
`49 8b 77 18` loads terrain+0x18; the coordinate calculation multiplies by
nx at grid+8; `48 8b 46 10` reads records at grid+0x10; two LEAs compute
40-byte records; `45 89 2c 24` stores the entity at record+0. These are existing
contracts, not newly selected patch sites. Runtime byte refusal is unchanged.

Native regression uses the real FindRecord and LoadHook against synthetic
terrain, including surviving threads and a reused address. It tests probe
complexity, not native game scheduling or cache lifetime. No live run was
performed; experimental sidecars remain default off and earlier live-proof
gaps remain open. No Windows timing is attributed to Linux.

Local evidence: `.git/port-5fd49a2-addtile.asm`,
`.git/port-5fd49a2-build.log`, `.git/port-5fd49a2-lua.log`.
