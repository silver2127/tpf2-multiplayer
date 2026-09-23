# Upstream dev d3135a59 integration

Windows target: `d3135a591c38729a94ff576ca388099ba3b477c1`.
Baseline: [38432b5f](UPSTREAM_dev_38432b5f.md), native Linux build 35924.
The merge remains staged and uncommitted. No conflicts were present.

## Merged

All ten commits: `77fb28b`, `6a31e0f`, `9fd6c88`, `c6fb8ac`, `8d9b517`,
`97c27f8`, `86bfc47`, `2e62429`, `364358b`, `d3135a5`.
Windows native paths are preserved unchanged from the incoming revision.

All 28 Lua files exactly match the target. This includes persistent founders
and ordinal company names, paint scheduling after every buy and colour retries,
station permissions and line rechecks, forced hash cadence, foreign-edit
capture guards, and the company-window stylesheet. Shared tests/docs merge
unchanged. Lua verifier and release provenance now name this target.
Manifest SHA-256: `760005868a5c7b055500cf9c31820854dec28365d7e041f145b2cc7c43ab7e70`.

## Ported

- Menu note moved into the JOIN column beside the separate-companies checkbox;
  shortened public-games header. Existing Linux field spacing is retained.
- Station editor permissions: existing SysV hook now reads the same bounded
  permission file as Windows, with a mutex-protected two-second cache and
  matching default-open behavior. Lua enforces authoritative line permissions.
- SHOW ALL ICONS: four proven Linux branches cover Visit, both End component
  storage paths, and the HUD worker. Windows' three NOPs do not translate
  directly: Linux has two equality branches requiring unconditional jumps.
- Foreign station/vehicle/depot info-window eligibility: verified Linux
  ViewCreator predicate, six-byte rejection-branch NOP, existing native command
  barrier plus merged Lua foreign-edit filter. `showicons=0` and
  `foreignwindows=0` disable the respective native features.

All new sites are checked before patching, use the existing safe writer and
build-id gate, and have static ELF and synthetic tests.
[RE evidence and unresolved tint mappings](../re/linux/DEV_D3135A59.md).

## Not ported

Vehicle-icon tint, station-label company background, and native company-window
wash remain unavailable. Static analysis located the Linux owner gates, draw
helper candidates, label colour selection and entity-window ID binding, but
could not establish the complete colour ABI/live-register flow and native
style-class/string contract. The RE record lists exact attempts and missing
evidence for each. No guessed draw call, pointer lifetime or MSVC string layout
is installed. The shared stylesheet alone cannot colour native windows.

Earlier native recovery, automatic in-world client loading, company-name
capture/ownership, native updater and Workshop gaps remain. This integration
does not establish live gameplay compatibility. Neither Steam nor the game was
started, and no real installation or publishing action was performed.

## Tests

- `tools/linux/build_native.sh`: pinned soldier build, **44/44 CTests**, glibc
  <=2.31 validation passed. Movement tests now cover permission parsing,
  mutated UI guards, group preflight refusal and all five patch replacements.
- `python3 tools/linux/verify_lua_release.py`: **28 exact files**, manifest above.
- `verify_dev_d3135a59_elf.py`: build-id, ten byte spans, five branch edits,
  instruction boundaries/destinations, no interior targets, RTTI/vtable passed
  against the actual game ELF. Earlier `verify_dev_38432b5f_elf.py` also passed.
- `company_name_test.py`, `clone_buy_test.py`, `company_perms_test.py`,
  `foreign_edit_guard_test.py`, `hash_cadence_test.py`: all passed with Lua 5.2
  via Lupa in a disposable `.git/port-venv` (no system installation).
- Release shell syntax, Windows source equality, merge-state and whitespace
  checks passed. Logs: `.git/port-*.log`.

The first native build exposed a const-pointer conversion error in the new
writer calls; it was fixed before the passing build. No commit or merge abort.
