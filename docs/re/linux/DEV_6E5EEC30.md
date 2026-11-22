# Native input policy for dev 6e5eec30

This integration changes an existing SDL filter's policy. It introduces no
ELF address, patch bytes, struct offset, calling convention or ownership claim.
No new reverse engineering or live ABI probe is required.

Windows `NativeIo::inputProc` now tests `actionsHeld && inputBlocking`.
Linux `panel::EventFilter` now tests `g_actionsHeld && g_flagInputHold` before
consuming keyboard, mouse and text events. The flag defaults false and only
`input_hold=1` enables it, matching Windows parsing. Linux reads the existing
flags file beside `tpf2_menu.so`; reads and use share the panel mutex. The
setting is logged. No exported setter is needed: its only upstream caller is
startup flag parsing, which Linux already owns in the panel.

Physical key/button tracking and `panel::SetActionsHeld` are unchanged: a new
hold fails while a non-Escape key or mouse button is down or when the SDL
filter is unavailable. Normal overlay capture and previous-filter chaining
remain in place.

The legacy script hook is deliberately unchanged. Windows
`legacyScriptEventHook` still checks only `actionsHeld`, as does Linux
`NativeIo::LegacyEventHook`. This gate suppresses the guide script's recurring
saveevent command producer so pause/drain can finish; it is not the game-window
input filter. Its existing byte-verified site and SysV trampoline are not
modified. The prior mapping is documented in [SLICE_CORE.md](SLICE_CORE.md).

`panel_title` exercises the actual SDL filter with a closed overlay: keyboard,
mouse motion/buttons/wheel, text/composition, Escape, non-input events, flag
parsing and gesture-release gating. `native_io` confirms legacy events forward
their arguments/return value before and after a hold and do not call the engine
while held. These are fixture observations, not a running-game result.
