# Native save input gate for dev 6e1e4ec7

This is a policy change to existing native code, following
[DEV_6E5EEC30.md](DEV_6E5EEC30.md). No game address, patch bytes, object offset,
foreign calling convention or lifetime contract is added or changed. No new
ELF mapping or live ABI probe is needed.

Windows `NativeIo::inputProc` now suppresses input when actions are held and
`inputBlocking || savingNow()`. Its reason is a Windows Big Maps pager
deadlock observed upstream when camera movement faulted terrain tiles during
sidecar saving. This integration does not claim to reproduce that deadlock
on Linux or fix the pager itself.

Linux `panel::EventFilter` uses the equivalent predicate:
`g_actionsHeld && (g_flagInputHold || NativeIo::SavingNow())`.
`SavingNow` reads the existing `Control::state` under `std::try_to_lock`;
contention conservatively returns true for this event. The SDL filter holds
the panel mutex, so waiting on the controller mutex here would be undesirable.
The helper never waits for it or calls the engine. The existing panel-before-
controller path in `SetActionsHeld` releases the panel lock before returning.

`Request` sets `QueuedSave` before returning; `Tick` changes it to `Saving`
before invoking the engine save. Matching save completion (success or failure),
queue failure, and world-change handling return the state to `Idle`. The helper
therefore covers queued work through completion, but not load or pause states.
`Busy()` would incorrectly block input for those other states and would wait
on the mutex, so it is not used.

Existing SDL input classes, Escape handling, gesture tracking, overlay capture,
clipboard re-entry and previous-filter chaining retain their prior behavior.
Legacy script-event suppression remains tied to the round hold. This change
adds no new interception site; existing build-id and byte checks are retained.

## Fixture evidence

- `native_io`: checks every state, a real second thread querying while the
  state mutex is owned, release after contention, unrelated/matching save
  callbacks, failed completion and queue-failure cleanup.
- `panel_title`: exercises the actual SDL filter with a stubbed save query;
  keyboard, mouse and text input are consumed during a held save with the flag
  off, then resume after saving; an unheld save does not suppress input.
  Escape and non-input events still pass; the opt-in whole-round block and
  physical gesture gates remain covered.

No game was launched or attached to for this integration. These are fixture
results, not an observation of camera movement or Big Maps saving on screen.
