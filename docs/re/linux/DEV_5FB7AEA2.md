# dev 5fb7aea2: HUD station/depot company wash (not ported)

Static investigation of Linux Steam build 35924. `readelf -n` confirms GNU
build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`. The game was read,
never executed. All addresses below are ELF virtual addresses, analysis leads
only; none is installed as a hook.

## Windows contract

The incoming post-call detour at RVA `0x5e38d0` consumes rax=content and
ebx=entity, replays `45 33 c9 41 b0 01`, and preserves the content pointer.
Owner lookup uses PlayerOwned directly for depots, or StationGroup's first
station for groups. `WindowTintApply` appends `!mpWinCoN` via the Windows
style method. Positive company IDs, including the local company, are tinted;
unowned/co-op remain unchanged. `stationicon=0` disables installation.
Neither Windows SEH nor its cached engine, type lookup ABI and MSVC string
can be assumed valid in the native port.

## Fresh Linux investigation

Read the existing [company UI evidence](DEV_D3135A59.md), searched functions,
signature and string-reference exports, and disassembled the complete
`HudIconManager::DoStep` at `0x1093b30` (13245 bytes). Its source/signature
anchors are in `funcsig.csv`. Followed these content-building call paths:

- `0x109494e`: `e8 0d ac ff ff` calls `0x108f560`. Its return is saved at
  rbp-0xf20 at `0x109495d`, then passed in rsi to `0x306fd80` at
  `0x1094983`. Disassembling the callee and checking its strings rejects this
  as the station hook: it references `TownItem` and `townhudicon`. RTTI at
  `0x5a01b88`, through the relocation at +8, names
  `N3ecs9component4TownE`, verified from the actual ELF.
- `0x1095972`: `e8 d9 a8 ff ff` calls `0x1090250` (2380 bytes).
  This is a station-content candidate: xrefs identify `StationItem`,
  `::StationIcon`, `::StationIcons`, and carrier glyph names. Inspected its
  complete disassembly. At `0x1095977`, `48 83 c4 20` removes four stack
  arguments; `0x1095985` (`48 89 85 98 f0 ff ff`) saves rax at rbp-0xf68.
  At `0x109599b` that result is loaded into rsi for the same `0x306fd80`
  call at `0x10959ab`. This establishes a content-to-wrapper flow, not the
  full Windows context/entity ABI: this call uses six register arguments
  plus four stack arguments. ebx is pushed as one of those arguments at
  `0x109594d`; it is not proven to be the icon's entity.
- Also disassembled `0x1092ca0..0x1092e3f`, called at `0x109515f`.
  This is an aggregate-output path, not a simple content pointer factory:
  rdi is saved in rbx, fields are written through rbx at +0 and +8..+0x30,
  and rax returns rbx (`48 89 d8` at `0x1092dd6`). Its two component slot
  lookups at `0x1092cdf` and `0x1092d39` use `0x9e5590`. Their observed
  component strides alone do not identify StationGroup/PlayerOwned.

Continued style-method investigation using all three `addStyleClass` string
xref locations. At `0x1cad7fd`, `48 8d 15 bf 3d 29 02` loads the name at
`0x3f415c3` (verified literal `addStyleClass\0`); `0x1cad80b` stores it at
rbx+0x1d8 in a binding table. The second xref at `0x1cb1618`
(`48 8d 35 a4 ff 28 02`) passes the name to `0x1c974e0`, followed by
`0xd31210` and temporary-string cleanup. This is binding construction,
not evidence that either callee is CComponent::addStyleClass. The third
xref remains in the binding region `0x1ce37d0`.

## Decision and missing proof

The attempt did not establish a complete safe implementation. Missing:

- A station **and depot** insertion site with proven entity and live-register
  flow, instruction boundaries and no interior branch targets.
- The StationGroup type/accessor, first-station layout, and PlayerOwned
  lookup using the correct live UI engine across world changes.
- The native style-class method and its string ownership/exception contract.

Keep stationicon unavailable, along with the pre-existing iconcolor and
windowcolor gaps. The Linux startup diagnostic now explicitly names it.
No speculative patch, struct access, Windows address or fake kill switch is
added. Shared stylesheet availability alone does not apply the class.

Disposable captures are `.git/port-5fb7aea-{hud,item,content,station,style}.asm`.
The evidence above persists independently of those files. No runtime UI or
cross-platform compatibility claim is made.
