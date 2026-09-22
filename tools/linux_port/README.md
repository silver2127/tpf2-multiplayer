# tpf2-port: Windows commits ported to Linux automatically

Every push to `dev` from this PC, in `tpf2-multiplayer` or `tpf2-bigmap`, is sent to the Linux machine
(`topsnek@strelka`). Codex ports it there, with Claude as the fallback. The result is a pull request
into `linux-native`.

```
git push origin HEAD:dev (or a fetch that moves origin/dev)
  -> .git/hooks/reference-transaction (returns at once; only refs/remotes/origin/dev counts)
  -> tpf2-port-push.sh (detached): git push --force origin/dev as dev
       to strelka:~/tpf2-port/inbox/<repo>.git; retries with backoff up to 8 times
  -> inbox post-receive: setsid port_worker.py (one at a time: flock state/worker.lock)
  -> per branch, oldest first, one job at a time:
       clone the GitHub mirror, check out port/<branch> (or linux-native),
       merge linux-native into an existing port branch when that is clean,
       merge the new Windows commits (at most 20 per job) without committing,
       codex exec --dangerously-bypass-approvals-and-sandbox in a fresh session (3 rounds; the follow-up
         rounds resume that job's own session, whose id is read from the `session id:` log line), then
       claude -p --dangerously-skip-permissions (3 rounds);
       after every round the WORKER runs the verification itself and feeds a failure back
  -> commit, push port/<branch>, open or update the PR into linux-native:
       ready when verification passed, draft when it did not
```

| File | Where it runs |
|---|---|
| `install_windows_hooks.ps1` | Windows. Installs the hook in each repo's common `.git\hooks` (covers every worktree) and pushes once. `-Uninstall` removes it. `$env:TPF2_PORT_OFF=1` skips it for one shell |
| `setup_laptop.sh` | Linux. Installs `~/tpf2-port` (worker, prompt, config, one bare inbox per repo). Idempotent; `--config` overwrites `config.json` |
| `port_worker.py` | Linux. `--plan` lists pending jobs, `--once`, `--no-publish`, `--baseline` |
| `prompt.md` | The agents' instructions: port rules, never invent Linux addresses, never push or start the game, write `REPORT.md` and `STATUS` |
| `config.json` | Agents, verification commands per repo, excluded branches, limits |
| `test_port_worker.py` | Linux, offline: a local stand-in for GitHub, shell-script agents, no tokens |

## What gets ported

- A repository's first push records every branch tip as the **baseline**. A branch is ported only
  after its tip moves, and then everything on it that is not yet in `linux-native` or `port/<branch>`.
- Only `dev` is ported: the hook sends nothing else (`install_windows_hooks.ps1 -Branches`, default `dev`),
  and the worker ignores anything outside `include` in `~/tpf2-port/config.json` (`["dev"]`). Agents push
  their commits to `dev` (`~/.claude/CLAUDE.md`).
- A job that errors (for example GitHub is unreachable) is retried at most twice for the same tip. A
  port that fails verification is still pushed, as a draft PR, so later jobs build on it and must make
  it pass.

## Codex sessions

Each job starts a fresh Codex session (2026-09-16 16:40, after two jobs that resumed the user's main
session cost about 3.6M tokens each, almost all of it reloading that session's 196 MB history). The
follow-up rounds of a job resume the session its first round opened. The worker still supports a fixed
`session` with `session_lock` / `session_wait_minutes` (a Codex window holding the session's writer lock
makes the job wait, then fall through to the next agent), but config.json no longer sets one.

## Publishing

Port branches and PRs go to the public GitHub repos with the laptop's `gh` login. That includes the
merged Windows commits of `dev`.

There is **one pull request per branch** (`port/dev` -> `linux-native`, #5 in tpf2-multiplayer and #4
in tpf2-bigmap); it is never closed by the worker. Every run pushes more commits to it and rewrites its
description: latest result, how many Windows commits over how many runs, a table of every run, the
latest agent report. A comment (so a notification) is posted only when verification failed or when it
passes again after a failure. Until 2026-09-17 every run commented the whole report instead, which made
one PR look like eighteen.

## Where to look

- Windows: `.git/tpf2-port/push.log` in each repo.
- Linux: `~/tpf2-port/state/worker.out` (the queue), `~/tpf2-port/jobs/<job>/meta/` (prompt, agent
  logs, verification logs, `REPORT.md`, `PR_BODY.md`). The newest 8 jobs are kept.
- Desktop notification on the laptop after each job.

## Failure modes seen

- `push.log` repeating `fatal: Unable to read current working directory` (2026-09-16, twice, ~90 min each):
  the pusher inherited the directory of the `git fetch` that fired the hook, and that directory was
  removed (a temporary clone, a deleted worktree). It now `cd`s to the common git dir first. While a
  pusher retries, later hook runs only set the dirty flag, so nothing reaches the laptop until it
  gives up (8 tries) or succeeds.
- Codex out of usage credits (`codex-N.log` ends with "You've hit your usage limit"): the round fails
  in seconds and the worker falls through to Claude, which ports and passes on its own (bigmap PR #4,
  2026-09-16 23:23). Nothing to do but wait for the quota window.

## Live testing and backlog runs (2026-09-17)

The agents may run the game, only through the two-instance lab on the laptop (`tools/sandbox/tpf2mp-lab run
native|proton`, `docs/sandbox/INSTALL.md` on `linux-native`): its own game, saves and mod copies; Steam stays
untouched. The worker passes the desktop session (`systemctl --user show-environment`: DISPLAY, WAYLAND_DISPLAY,
XDG_RUNTIME_DIR) to the agents, and `prompt.md` has a "Live testing" section (gdb attach via passwordless
sudo, hygiene, restore the actor when done). There is no input automation on the laptop (Wayland, no
xdotool): the agents drive the game through the menu flags (`autoload=1`), saves and the lobby programs.

A **backlog run** works on what earlier runs left unported, without new Windows commits:

```
python3 ~/tpf2-port/port_worker.py --revisit tpf2-multiplayer --focus ~/tpf2-port/revisit/tpf2-multiplayer.md
```

It waits for a running job, clones the current `port/dev`, hands the agents the focus list (`revisit/<repo>.md`
here, the open items with the addresses already located), verifies and publishes like any run (the runs table
marks it "(revisit)"). Start it by hand when the list is worth another attempt; it is not triggered by pushes.
