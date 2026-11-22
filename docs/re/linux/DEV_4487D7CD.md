# Gameplay UI timestamp export (dev 76fd191 / release 4487d7cd)

## Linux mapping and contract

The Windows relay stamps GetTickCount64 on CGameUI updates and clears it on
CreatePage(2). Native Linux reuses the existing hooks documented in
[MENU_GAME](MENU_GAME.md), sections 9.1–9.2. It stamps CLOCK_MONOTONIC
milliseconds in GameUiUpdateDetour only when the object has the verified
CGameUI vtable and equals the existing global current-game UI. As with the
existing OnGameUiFrame signal, nested updates count too. No new patch, field,
object ownership, or calling convention is introduced.

The C ABI `uint64_t Tpf2mpLastGameUiTick()` returns an atomic relaxed snapshot;
it exposes no game pointers and performs no engine access on reader threads.
The initial value and the value after title page 2 are zero. Other pages,
including loading page 16, do not clear it. A fresh current-world update
replaces it. The value is Linux monotonic time, not Windows uptime; consumers
must compare against the same Linux clock. Relaxed ordering is sufficient:
this is a timestamp, not publication of game-owned state.

The menu is loaded RTLD_LOCAL. Consumers must dlsym its module handle (e.g.
obtained with dlopen of the loaded module path and RTLD_NOLOAD), not assume
RTLD_DEFAULT sees it. exports_menu.map publishes only this C function and
keeps libstdc++ implementation symbols hidden.

## Static evidence rechecked in this integration

Lab ELF GNU build ID: `3a0e156390b0e6f1e372051c24802c8493ae454a` (35924).
Addresses below are ELF virtual addresses, rebased by the existing loader.

- RTTI name at 0x410fa98: `N2UI7CGameUIE`; typeinfo 0x5a11248 points
  to that name. Vtable 0x5a11468 has this typeinfo at slot -1.
- Slot 34 at 0x5a11578 points to 0x100fb20. Its verified 23-byte prologue:
  `f3 0f 1e fa 55 48 89 e5 41 57 41 56 49 89 d6 41 55 41 54 53 48 89 fb`.
- Step2 at 0x305a7a8 moves r12 -> rdx, r13 -> rsi, r15 -> rdi,
  then calls rax at 0x305a7b1. SysV signature remains
  `void(void* self, int64_t t, int64_t dt)`.
- Global current UI 0x5a4fb38 is written by `48 89 3d 48 d1 a6 04`
  at 0xfe29e9 and read by the conditional getter at 0xfe2a35.
- CreatePage remains 0x1154b20, SysV `(rdi=menu, esi=page)`. Rechecked
  prologue: `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55 41 54`.

Existing build-ID, prologue, RTTI/vtable, global-reference, and frame-gate
checks still gate hook installation. Failed checks do not introduce a
fallback address or an unguarded hook. The new stamp uses that existing path.
Raw static checks/disassembly are in the job's `meta/live/static-elf.log`.

## Native pager scope

`bigmap/linux/terrain_pager.h` uses userfaultfd, MemAvailable-based resident
budgets and fault recency. It has neither the Windows three-minute loading
tail nor its per-fault commit throttle, and currently consumes no UI stamp.
No Windows throttle or stall-based eviction policy is added by this export
fix. The Windows timing improvement in release notes is not a measured Linux
improvement.

## Live attempt and tests

Both lab payloads were backed up to unique `.before-port-4487d7cd` siblings;
the built libraries and merged Lua were installed for the launch attempt.
The prescribed native launch exited 1 at `bwrap: setting up uid map:
Permission denied`, before the game process existed. No gdb attach, frame,
world transition, GPU selection or performance result was observed. Both
payloads were restored and no game was left running. Logs are in the job's
`meta/live/`. No new lifetime interpretation relies on this failed run.

Synthetic tests exercise the real update detour with current, non-current
and wrong-vtable objects, monotonic refresh, and the real CreatePage detour's
reset/preservation rules. They do not establish live game behavior. A real
RTLD_LOCAL dlopen/dlsym of the built library returns zero before any frame;
nm reports only Tpf2mpLastGameUiTick as a defined dynamic export.
