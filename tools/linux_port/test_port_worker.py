#!/usr/bin/env python3
"""Offline test of port_worker.py: a local bare repository stands in for GitHub,
a working repository for the Windows machine, shell scripts for the agents, and
the "record" publisher for pull requests. No network, no tokens.

  python3 tools/linux_port/test_port_worker.py
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def sh(cwd, *args):
    r = subprocess.run(list(args), cwd=cwd, capture_output=True, text=True)
    assert r.returncode == 0, (args, r.stdout, r.stderr)
    return r.stdout.strip()


def git(cwd, *args):
    return sh(cwd, 'git', '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'init.defaultBranch=main', *args)


def commit(repo, name, text, message):
    (Path(repo) / name).write_text(text)
    git(repo, 'add', name)
    git(repo, 'commit', '-q', '-m', message)


FIXER = r'''#!/bin/sh
# resolves conflicts by taking the Windows side, "ports" by writing ported.txt
meta="$1"
for f in $(git diff --name-only --diff-filter=U); do git checkout --theirs -- "$f"; git add "$f"; done
echo ok > ported.txt
printf '## Merged\nall\n## Ported\nported.txt\n## Not ported\nnothing\n## Tests\nverify\n' > "$meta/REPORT.md"
echo DONE > "$meta/STATUS"
'''
DEAD = '#!/bin/sh\necho "quota exhausted" >&2\nexit 3\n'
REVISITOR = r'''#!/bin/sh
# a backlog run: nothing to merge; the prompt (stdin) names the items; "ports" one and keeps verification green
meta="$1"
cat > "$meta/prompt-seen.md"
echo ok > ported.txt
echo backlog > backlog.txt
printf '## Merged\nnone (revisit)\n## Ported\nbacklog.txt\n## Not ported\nrest\n## Live testing\nran the lab\n## Tests\nverify\n' > "$meta/REPORT.md"
echo PARTIAL > "$meta/STATUS"
'''
WRONG = '#!/bin/sh\necho broken > ported.txt\necho DONE > "$1/STATUS"\n'


def main():
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        home = td / 'home'
        home.mkdir()
        # GIT_DIR=. is what the inbox's post-receive hook hands the worker (2026-09-16: every
        # job clone then failed with "not a git repository: '.'").
        env = dict(os.environ, TPF2_PORT_HOME=str(home), GIT_DIR='.', GIT_QUARANTINE_PATH=str(td / 'quarantine'))
        for name, text in (('fixer', FIXER), ('dead', DEAD), ('wrong', WRONG), ('revisitor', REVISITOR)):
            p = td / name
            p.write_text(text)
            p.chmod(0o755)

        # "GitHub": main and linux-native
        seed = td / 'seed'
        seed.mkdir()
        git(seed, 'init', '-q')
        commit(seed, 'README', 'base\n', 'base')
        git(seed, 'branch', 'linux-native')
        github = td / 'github'
        github.mkdir()
        git(td, 'clone', '-q', '--bare', str(seed), str(github / 'demo.git'))

        # "Windows": a clone with an old branch that must stay unported
        win = td / 'win'
        git(td, 'clone', '-q', str(github / 'demo.git'), str(win))
        git(win, 'checkout', '-q', '-b', 'old')
        commit(win, 'old.txt', 'old\n', 'old work')
        git(win, 'checkout', '-q', 'main')

        def config(agents, **extra):
            cfg = {
                'github_url': f'{github}/{{repo}}.git', 'publish': 'record', 'max_commits': 20, 'rounds_per_agent': 2,
                'keep_jobs': 50, 'agents': [{'name': a, 'cmd': [str(td / a), '{meta}']} for a in agents],
                'repos': {'demo': {'verify': [['sh', '-c', 'grep -qx ok ported.txt']]}},
            }
            cfg.update(extra)
            (home / 'config.json').write_text(json.dumps(cfg))

        (home / 'prompt.md').write_text((HERE / 'prompt.md').read_text())
        inbox = home / 'inbox' / 'demo.git'
        inbox.parent.mkdir(parents=True)
        git(td, 'init', '-q', '--bare', str(inbox))

        def push():
            git(win, 'push', '-q', '--force', '--prune', str(inbox), '+refs/heads/*:refs/heads/*')

        def worker(*args):
            r = subprocess.run([sys.executable, str(HERE / 'port_worker.py'), *args], env=env,
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            return r.stdout

        def published():
            p = home / 'state' / 'published.jsonl'
            return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

        def github_file(branch, name):
            return subprocess.run(['git', '-C', str(github / 'demo.git'), 'show', f'{branch}:{name}'],
                                  capture_output=True, text=True).stdout

        config(['dead', 'fixer'])
        push()
        worker()                                        # first push: baseline only
        baseline = json.loads((home / 'state' / 'baseline.json').read_text())
        assert set(baseline['demo']) == {'main', 'old'}, baseline
        assert worker('--plan') == '' and published() == []

        # 1. A new branch; the first agent is unusable, the second one ports it.
        git(win, 'checkout', '-q', '-b', 'feat')
        commit(win, 'a.txt', 'a\n', 'feat: a')
        git(win, 'checkout', '-q', '-b', 'worktree-agent-x')
        commit(win, 'x.txt', 'x\n', 'scratch')
        git(win, 'checkout', '-q', 'feat')
        push()
        plan = worker('--plan')
        assert 'feat' in plan and 'worktree-agent-x' not in plan and 'old' not in plan, plan
        worker()
        events = published()
        assert [(e['head'], e['action'], e['draft']) for e in events] == [('port/feat', 'create', False)], events
        assert github_file('port/feat', 'a.txt') == 'a\n' and github_file('port/feat', 'ported.txt') == 'ok\n'
        assert 'dead round 1: exited 3 without changing anything' in events[0]['body'], events[0]['body']
        assert 'fixer round 1: status DONE, verification passed' in events[0]['body']
        assert github_file('linux-native', 'a.txt') == ''          # linux-native itself is never pushed
        prompt = next((home / 'jobs').glob('demo-feat-*')) / 'meta' / 'prompt.md'
        assert 'feat: a' in prompt.read_text() and '{{' not in prompt.read_text()

        # 2. linux-native moves on. The existing port branch catches up before the next port,
        #    and a new branch whose change conflicts with it gets the conflict resolved.
        ln = td / 'ln'
        git(td, 'clone', '-q', '-b', 'linux-native', str(github / 'demo.git'), str(ln))
        (ln / 'README').write_text('linux\n')
        commit(ln, 'ln.txt', 'linux only\n', 'linux edit')
        git(ln, 'commit', '-q', '-a', '-m', 'linux readme')
        git(ln, 'push', '-q', 'origin', 'linux-native')
        commit(win, 'c.txt', 'c\n', 'feat: c')
        push()
        worker()
        events = published()
        assert [(e['head'], e['action'], e['draft']) for e in events[1:]] == [('port/feat', 'update', False)], events
        assert events[-1]['comment'] is None, events[-1]          # passed again: the description changes, nobody is pinged
        assert '| Ported so far | 2 Windows commit(s) over 2 run(s)' in events[-1]['body'], events[-1]['body']
        assert github_file('port/feat', 'c.txt') == 'c\n' and github_file('port/feat', 'ln.txt') == 'linux only\n'
        git(win, 'checkout', '-q', '-b', 'feat3', 'main')
        commit(win, 'README', 'windows\n', 'feat3: readme')
        push()
        worker()
        events = published()
        assert (events[-1]['head'], events[-1]['action'], events[-1]['draft']) == ('port/feat3', 'create', False), events
        assert github_file('port/feat3', 'README') == 'windows\n'
        assert '`README`' in events[-1]['body'], events[-1]['body']

        # 3. No agent gets it right: pushed as a draft, and not retried for the same tip.
        config(['dead', 'wrong'])
        git(win, 'checkout', '-q', '-b', 'feat2', 'main')
        commit(win, 'b.txt', 'b\n', 'feat2: b')
        push()
        worker()
        events = published()
        assert (events[-1]['head'], events[-1]['action'], events[-1]['draft']) == ('port/feat2', 'create', True), events[-1]
        assert 'Verification failed' in events[-1]['body'] and 'wrong round 2' in events[-1]['body']
        assert worker('--plan') == ''                 # the failed merge is on port/feat2, so nothing is pending

        # 4. A long branch is split into jobs of max_commits.
        config(['fixer'], max_commits=2)
        git(win, 'checkout', '-q', '-b', 'long', 'main')
        for i in range(3):
            commit(win, f'l{i}.txt', f'{i}\n', f'long {i}')
        push()
        worker()
        long_events = [e for e in published() if e['head'] == 'port/long']
        assert [(e['action'], e['draft']) for e in long_events] == [('create', False), ('update', False)], long_events
        assert 'more follow in the next run' in long_events[0]['body']
        assert github_file('port/long', 'l2.txt') == '2\n'

        # 5. One worker at a time: while one holds the lock, another started by a push does nothing;
        #    the queue is worked through once the lock is free.
        import fcntl
        git(win, 'checkout', '-q', '-b', 'queued', 'main')
        commit(win, 'q.txt', 'q\n', 'queued')
        push()
        before = len(published())
        with open(home / 'state' / 'worker.lock', 'w') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            assert worker() == '' and len(published()) == before      # exited at once, ran nothing
        worker()
        assert [e['head'] for e in published()[before:]] == ['port/queued'], published()[before:]

        # 6. The old baseline branch is still untouched, and the scratch branch was never ported.
        assert not any(e['head'] in ('port/old', 'port/worktree-agent-x') for e in published())

        # 6b. An agent that resumes a saved session: while another process holds the session's
        #     writer lock it is skipped after its wait, and the next agent does the job; once the
        #     lock is free it runs, resumed by id, with its prompt prefix.
        session_lock = td / 'sessions' / 'S-123.lock'
        session_lock.parent.mkdir()
        session_lock.touch()
        (td / 'sess').write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$1/args.txt"\ncat > "$1/stdin.txt"\n'
                                 'exec "' + str(td / 'fixer') + '" "$1"\n')
        (td / 'sess').chmod(0o755)

        def session_config():
            config(['fixer'])
            cfg = json.loads((home / 'config.json').read_text())
            cfg['agents'] = [{'name': 'sess', 'session': 'S-123', 'session_lock': str(td / 'sessions' / '{session}.lock'),
                              'session_wait_minutes': 0.002, 'prompt_prefix': '[automated]\n',
                              'cmd': [str(td / 'sess'), '{meta}', 'resume', '{session}']},
                             {'name': 'fixer', 'cmd': [str(td / 'fixer'), '{meta}']}]
            (home / 'config.json').write_text(json.dumps(cfg))

        session_config()
        git(win, 'checkout', '-q', '-b', 'held', 'main')
        commit(win, 'h.txt', 'h\n', 'held')
        push()
        before = len(published())
        with open(session_lock, 'a') as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            worker()
        body = published()[before]['body']
        assert 'sess round 1: session S-123 stayed open elsewhere' in body and 'fixer round 1' in body, body
        git(win, 'checkout', '-q', '-b', 'free', 'main')
        commit(win, 'f.txt', 'f\n', 'free')
        push()
        worker()
        body = published()[-1]['body']
        assert published()[-1]['head'] == 'port/free' and 'sess round 1: status DONE' in body, body
        free_meta = next((home / 'jobs').glob('demo-free-*')) / 'meta'
        assert (free_meta / 'args.txt').read_text().split() == [str(free_meta), 'resume', 'S-123']
        assert (free_meta / 'stdin.txt').read_text().startswith('[automated]\n')

        # 6c. A fresh session per job: round 1 starts a new session and prints its id; round 2
        #     resumes THAT id (read from the log), not a configured or most-recent one.
        (td / 'fresh').write_text('#!/bin/sh\nmeta="$1"; shift\nprintf "%s\\n" "$@" >> "$meta/fresh-args.txt"\n'
                                  'cat > /dev/null\n'
                                  'if [ "$1" = resume ]; then exec "' + str(td / 'fixer') + '" "$meta"; fi\n'
                                  'echo "session id: 01a0ffff-1111-2222-3333-444444444444"\n'
                                  'echo broken > ported.txt\necho DONE > "$meta/STATUS"\n')
        (td / 'fresh').chmod(0o755)
        config(['fixer'])
        cfg = json.loads((home / 'config.json').read_text())
        cfg['agents'] = [{'name': 'fresh', 'cmd': [str(td / 'fresh'), '{meta}', 'new'],
                          'session_from_log': '^session id: ([0-9a-f-]{36})$',
                          'continue_cmd': [str(td / 'fresh'), '{meta}', 'resume', '{session}']}]
        (home / 'config.json').write_text(json.dumps(cfg))
        git(win, 'checkout', '-q', '-b', 'fresh', 'main')
        commit(win, 'n.txt', 'n\n', 'fresh')
        push()
        worker()
        ev = published()[-1]
        assert (ev['head'], ev['draft']) == ('port/fresh', False), ev
        assert 'fresh round 1: status DONE, verification `sh -c grep -qx ok ported.txt` exited 1' in ev['body'] and 'fresh round 2: status DONE, verification passed' in ev['body'], ev['body']
        fresh_meta = next((home / 'jobs').glob('demo-fresh-*')) / 'meta'
        assert (fresh_meta / 'fresh-args.txt').read_text().split() == ['new', 'resume', '01a0ffff-1111-2222-3333-444444444444']

        # 7. include limits porting to main: a new commit elsewhere is ignored, one on main is ported.
        config(['fixer'], include=['main'])
        git(win, 'checkout', '-q', 'feat')
        commit(win, 'ignored.txt', 'i\n', 'feat: ignored')
        git(win, 'checkout', '-q', 'main')
        commit(win, 'm.txt', 'm\n', 'main: m')
        push()
        plan = worker('--plan')
        assert 'main' in plan and 'feat' not in plan, plan
        before = len(published())
        worker()
        assert [e['head'] for e in published()[before:]] == ['port/main'], published()[before:]

        # 8. A revisit: no new Windows commits; the agents work on the backlog of the existing port branch,
        #    the prompt carries the focus list, the run lands on the same pull request as an update.
        config(['revisitor'], include=['main'])
        focus = td / 'focus.md'
        focus.write_text('- **native tinting**: owner lookup unproven\n')
        assert worker('--plan') == ''                   # nothing pending, and a revisit needs nothing pending
        before = len(published())
        out = worker('--revisit', 'demo', '--branch', 'main', '--focus', str(focus))
        assert 'revisit of the backlog' in out and 'port passed (revisitor)' in out, out
        ev = published()[before:]
        assert [(e['head'], e['action'], e['draft']) for e in ev] == [('port/main', 'update', False)], ev
        assert 'revisit of the backlog on top of' in ev[0]['body'] and '(revisit)' in ev[0]['body'], ev[0]['body']
        assert github_file('port/main', 'backlog.txt') == 'backlog\n' and github_file('port/main', 'm.txt') == 'm\n'
        seen = next((home / 'jobs').glob('demo-main-revisit-*')) / 'meta' / 'prompt-seen.md'
        seen = seen.read_text()
        assert 'REVISIT run' in seen and 'native tinting' in seen and 'Live testing' in seen and '{{' not in seen, seen
        assert worker('--plan') == ''                   # the revisit commit did not create pending work
        print('PASS: tpf2-port worker: baseline on first push; new branches ported with agent fallback; conflicts '
              'resolved and the single pull request description rewritten (a comment only on failure or recovery); a failed port lands as a draft and is not retried; long branches '
              'split by max_commits; one worker at a time (a second exits at the lock, the queue runs after); excluded '
              'and baseline branches untouched; a saved session is resumed by id when its lock is free and skipped for '
              'the next agent while held; a fresh session per job with follow-up rounds resuming that job\'s own session; '
              'include limits porting to main; a --revisit run works the backlog on the existing port branch')


if __name__ == '__main__':
    main()
