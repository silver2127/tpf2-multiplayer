#!/usr/bin/env python3
"""tpf2-port worker: ports new Windows commits to the native Linux branch.

The Windows machine pushes every local branch into <home>/inbox/<repo>.git
(install_windows_hooks.ps1). That repository's post-receive hook starts this
worker. For every watched branch with Windows commits that are neither in
linux-native nor in its port branch, one job:

  1. clones the GitHub mirror and checks out port/<branch> (or linux-native),
  2. merges the Windows commits without committing (at most max_commits),
  3. runs the agents in order (Codex, then Claude), both without sandbox or
     permission prompts. After each round the worker runs the repository's
     verification itself, so an agent's claim is never the gate, and hands a
     failure back,
  4. commits, pushes port/<branch> to GitHub and opens or updates a pull request
     into linux-native: ready when verification passed, draft when it did not.
     One pull request per branch: a later job rewrites its description (every run so
     far, the latest report) and comments only when verification failed or recovered,
     so the PR is not a stream of eighteen near-identical comments (2026-09-17).

  port_worker.py               drain the queue (what the post-receive hook runs)
  port_worker.py --plan        print the jobs that would run; change nothing
  port_worker.py --baseline    treat every current Windows tip as handled
  port_worker.py --once        run at most one job
  port_worker.py --no-publish  run jobs but push and open nothing
  port_worker.py --revisit REPO [--branch dev] [--focus FILE]
                               a backlog job: no new Windows commits; the agents work on what earlier
                               runs recorded as not ported (FILE names the items), on the current port
                               branch, with the same verification and pull request. Waits for a running
                               worker to finish. (2026-09-17: the agents may run the game in the lab now.)

The first push of a repository records its tips as the baseline: a branch is
only ported once its tip moves, so old, idle branches are left alone.
"""
import argparse
import fcntl
import fnmatch
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import traceback
from datetime import datetime
from pathlib import Path

HOME = Path(os.environ.get('TPF2_PORT_HOME', str(Path.home() / 'tpf2-port')))
STATE = HOME / 'state'
JOBS = HOME / 'jobs'

DEFAULTS = {
    'github_owner': 'silver2127',
    'github_url': 'https://github.com/{owner}/{repo}.git',
    'base_branch': 'linux-native',
    'include': ['*'],           # branch patterns considered at all; config.json limits this to main
    'exclude':['linux-native', 'port/*', 'gh-pages', 'worktree-*', 'release-*'],
    'max_commits': 20,
    'rounds_per_agent': 3,
    'agent_timeout_minutes': 120,
    'verify_timeout_minutes': 90,
    'max_failures': 2,
    'keep_jobs': 8,
    'publish': 'gh',            # gh | record (tests) | none
    'git_name': 'silver2127',
    'git_email': '52584484+silver2127@users.noreply.github.com',
    'agents': [],
    'repos': {},
}


class GitError(RuntimeError):
    pass


def log(msg):
    print(f'[{datetime.now():%Y-%m-%d %H:%M:%S}] {msg}', flush=True)


def git(repo, *args, check=True):
    r = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise GitError(f'git {" ".join(args)}: {(r.stderr or r.stdout).strip()}')
    return r.stdout.strip()


def load_json(name, default):
    p = STATE / name
    try:
        return json.loads(p.read_text())
    except (OSError, ValueError):
        return default


def save_json(name, value):
    STATE.mkdir(parents=True, exist_ok=True)
    tmp = STATE / (name + '.tmp')
    tmp.write_text(json.dumps(value, indent=2, sort_keys=True))
    tmp.replace(STATE / name)


def load_config():
    cfg = dict(DEFAULTS)
    cfg.update(json.loads((HOME / 'config.json').read_text()))
    return cfg


def inbox_path(repo):
    return HOME / 'inbox' / f'{repo}.git'


def mirror_path(repo):
    return HOME / 'work' / f'{repo}.git'


def github_url(cfg, repo):
    return cfg['github_url'].format(owner=cfg['github_owner'], repo=repo)


def refs_of(repo, prefix):
    out = git(repo, 'for-each-ref', '--format=%(refname)%09%(objectname)', prefix)
    refs = {}
    for line in out.splitlines():
        name, sha = line.split('\t')
        refs[name[len(prefix):].lstrip('/')] = sha
    return refs


def excluded(cfg, branch):
    if not any(fnmatch.fnmatchcase(branch, pat) for pat in cfg['include']):
        return True
    return any(fnmatch.fnmatchcase(branch, pat) for pat in cfg['exclude'])


def port_branch(branch):
    return f'port/{branch}'


def inbox_snapshot(cfg):
    h = hashlib.sha256()
    for repo in sorted(cfg['repos']):
        if inbox_path(repo).exists():
            h.update(repr(sorted(refs_of(inbox_path(repo), 'refs/heads').items())).encode())
    return h.hexdigest()


def refresh(cfg, repo):
    mirror = mirror_path(repo)
    if not mirror.exists():
        mirror.parent.mkdir(parents=True, exist_ok=True)
        git(HOME, 'clone', '--quiet', '--bare', github_url(cfg, repo), str(mirror))
        git(mirror, 'config', 'remote.origin.fetch', '+refs/heads/*:refs/heads/*')
    git(mirror, 'fetch', '--quiet', '--prune', 'origin')
    git(mirror, 'fetch', '--quiet', '--prune', str(inbox_path(repo)), '+refs/heads/*:refs/windows/*')


def record_baseline(cfg, only=None):
    baseline = load_json('baseline.json', {})
    for repo in cfg['repos']:
        if only is not None and repo != only:
            continue
        if inbox_path(repo).exists():
            baseline[repo] = refs_of(inbox_path(repo), 'refs/heads')
            log(f'{repo}: baseline of {len(baseline[repo])} Windows branch tip(s) recorded; only later commits are ported')
    save_json('baseline.json', baseline)


def plan(cfg):
    jobs = []
    baseline = load_json('baseline.json', {})
    attempts = load_json('attempts.json', {})
    base = cfg['base_branch']
    for repo in cfg['repos']:
        inbox = inbox_path(repo)
        if not inbox.exists():
            continue
        tips = refs_of(inbox, 'refs/heads')
        if not tips:
            continue
        if repo not in baseline:
            record_baseline(cfg, only=repo)
            continue
        refresh(cfg, repo)
        mirror = mirror_path(repo)
        heads = refs_of(mirror, 'refs/heads')
        if base not in heads:
            log(f'{repo}: GitHub has no {base} branch; skipped')
            continue
        for branch, tip in sorted(tips.items()):
            if excluded(cfg, branch) or baseline[repo].get(branch) == tip:
                continue
            tried = attempts.get(repo, {}).get(branch)
            if tried and tried['tip'] == tip and tried['failures'] >= cfg['max_failures']:
                continue
            port = port_branch(branch)
            exclude = [f'^refs/heads/{base}'] + ([f'^refs/heads/{port}'] if port in heads else [])
            pending = int(git(mirror, 'rev-list', '--count', f'refs/windows/{branch}', *exclude))
            if pending:
                when = int(git(mirror, 'log', '-1', '--format=%ct', f'refs/windows/{branch}'))
                jobs.append({'repo': repo, 'branch': branch, 'tip': tip, 'port': port, 'pending': pending, 'when': when})
    jobs.sort(key=lambda j: j['when'])
    return jobs


# ---- one job -------------------------------------------------------------------

def run_logged(cmd, cwd, log_path, timeout_s, stdin_text=None, env=None):
    """Run cmd in its own process group; return (exit code, timed out)."""
    with open(log_path, 'a') as out:
        out.write(f'$ {" ".join(cmd)}\n')
        out.flush()
        p = subprocess.Popen(cmd, cwd=str(cwd), stdout=out, stderr=subprocess.STDOUT, text=True,
                             stdin=subprocess.PIPE if stdin_text is not None else subprocess.DEVNULL,
                             start_new_session=True, env=env)
        try:
            if stdin_text is not None:
                p.stdin.write(stdin_text)
                p.stdin.close()
            return p.wait(timeout=timeout_s), False
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            out.write(f'\n[tpf2-port] killed after {timeout_s} s\n')
            return -9, True
        except BrokenPipeError:
            return p.wait(timeout=timeout_s), False


def tail(path, lines=120):
    try:
        text = Path(path).read_text(errors='replace').splitlines()
    except OSError:
        return ''
    return '\n'.join(text[-lines:])


def tree_state(wt):
    h = hashlib.sha256()
    h.update(git(wt, 'status', '--porcelain=v1', '--untracked-files=all').encode())
    h.update(git(wt, 'diff', 'HEAD', '--binary', check=False).encode())
    return h.hexdigest()


def tree_problems(wt, target):
    problems = []
    unmerged = git(wt, 'diff', '--name-only', '--diff-filter=U').split()
    if unmerged:
        problems.append('Unresolved merge conflicts (resolve, then `git add` each file): ' + ', '.join(unmerged))
    merging = git(wt, 'rev-parse', '-q', '--verify', 'MERGE_HEAD', check=False) != ''
    contained = subprocess.run(['git', '-C', str(wt), 'merge-base', '--is-ancestor', target, 'HEAD']).returncode == 0
    if not merging and not contained:
        problems.append(f'The merge of {target} was aborted. Run `git merge --no-ff --no-commit {target}` again and redo '
                        'the port on top of it; do not commit and do not abort it.')
    marked = []
    for name in git(wt, 'diff', 'HEAD', '--name-only', check=False).split():
        path = wt / name
        try:
            text = path.read_text(errors='strict')
        except (OSError, UnicodeDecodeError):
            continue
        if any(line.startswith(('<<<<<<< ', '>>>>>>> ')) for line in text.splitlines()):
            marked.append(name)
    if marked:
        problems.append('Conflict markers left in: ' + ', '.join(marked))
    return problems


def read_status(meta):
    try:
        return (meta / 'STATUS').read_text().split()[0].upper()
    except (OSError, IndexError):
        return ''


def verify(cfg, rcfg, wt, log_path):
    for cmd in rcfg.get('verify', []):
        rc, timed_out = run_logged(cmd, wt, log_path, cfg['verify_timeout_minutes'] * 60)
        if rc != 0:
            return False, f'`{" ".join(cmd)}` ' + ('timed out' if timed_out else f'exited {rc}')
    return True, 'passed'


def expand(args, wt, meta, session=''):
    return [a.replace('{workdir}', str(wt)).replace('{meta}', str(meta)).replace('{session}', session) for a in args]


def wait_for_session(agent):
    """An agent that resumes a saved session can only write to it while nothing else
    holds the session's writer lock (an open Codex window does). Wait for it."""
    lock = agent.get('session_lock')
    if not lock:
        return True
    path = Path(os.path.expanduser(lock.replace('{session}', agent.get('session', ''))))
    deadline = time.monotonic() + float(agent.get('session_wait_minutes', 30)) * 60
    announced = False
    while True:
        try:
            with open(path, 'a') as f:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                fcntl.flock(f, fcntl.LOCK_UN)    # free: released at once, the agent takes it
            return True
        except BlockingIOError:
            if time.monotonic() >= deadline:
                return False
            if not announced:
                log(f'  {agent["name"]}: session {agent.get("session")} is open elsewhere; waiting')
                announced = True
            time.sleep(max(0.01, min(30.0, deadline - time.monotonic())))
        except OSError:
            return True                           # no lock file: nothing holds the session


def run_agents(cfg, rcfg, job, wt, meta, prompt):
    history = []
    failure = None
    for agent in cfg['agents']:
        name = agent['name']
        if shutil.which(agent['cmd'][0]) is None:
            history.append(f'{name}: not installed')
            continue
        job_session = ''          # the session this job's first round opened (session_from_log)
        for rnd in range(1, cfg['rounds_per_agent'] + 1):
            before = tree_state(wt)
            if rnd > 1 and agent.get('continue_cmd') and (agent.get('session') or job_session):
                cmd, text = agent['continue_cmd'], followup_text(failure)
            else:
                cmd, text = agent['cmd'], prompt + previous_text(failure)
            text = agent.get('prompt_prefix', '') + text
            agent = dict(agent, session=agent.get('session') or job_session)
            if not wait_for_session(agent):
                why = (f'session {agent.get("session")} stayed open elsewhere for '
                       f'{agent.get("session_wait_minutes", 30)} min')
                history.append(f'{name} round {rnd}: {why}')
                failure = failure or {'agent': name, 'round': rnd, 'text': f'{name}: {why}.'}
                break
            (meta / 'STATUS').unlink(missing_ok=True)
            log(f'  {name} round {rnd}')
            rc, timed_out = run_logged(expand(cmd, wt, meta, agent.get('session', '')), wt, meta / f'{name}-{rnd}.log',
                                       cfg['agent_timeout_minutes'] * 60, stdin_text=text)
            if agent.get('session_from_log') and not job_session:
                m = re.search(agent['session_from_log'], tail(meta / f'{name}-{rnd}.log', 100000), re.M)
                if m:
                    job_session = m.group(1)
            status = read_status(meta)
            changed = tree_state(wt) != before
            if rc != 0 and not changed:
                why = 'timed out' if timed_out else f'exited {rc} without changing anything'
                history.append(f'{name} round {rnd}: {why}')
                failure = {'agent': name, 'round': rnd, 'text': f'{name} {why}.\n\n' + tail(meta / f'{name}-{rnd}.log', 40)}
                break                                   # unusable (quota, auth, crash): next agent
            problems = tree_problems(wt, job['target'])
            if problems:
                history.append(f'{name} round {rnd}: ' + '; '.join(problems))
                failure = {'agent': name, 'round': rnd, 'text': '\n'.join(problems)}
                continue
            if status == 'BLOCKED':
                history.append(f'{name} round {rnd}: reported BLOCKED')
                failure = {'agent': name, 'round': rnd,
                           'text': f'{name} reported BLOCKED:\n\n' + tail(meta / 'REPORT.md', 60)}
                break
            log_path = meta / f'verify-{name}-{rnd}.log'
            ok, summary = verify(cfg, rcfg, wt, log_path)
            history.append(f'{name} round {rnd}: status {status or "(none)"}, verification {summary}')
            if ok:
                return {'ok': True, 'agent': name, 'round': rnd, 'status': status, 'history': history,
                        'verify_log': log_path}
            failure = {'agent': name, 'round': rnd, 'text': f'Verification failed: {summary}\n\n```\n{tail(log_path)}\n```'}
    return {'ok': False, 'agent': failure['agent'] if failure else None, 'round': failure['round'] if failure else 0,
            'status': read_status(meta), 'history': history, 'failure': failure,
            'verify_log': None}


def previous_text(failure):
    if not failure:
        return ''
    return (f'\n\n## Previous attempt\n\n{failure["agent"]} (round {failure["round"]}) worked on this clone before you; '
            f'its changes are still in the tree. It ended with:\n\n{failure["text"]}\n')


def followup_text(failure):
    return ('The harness checked your work and it is not finished. Fix this, keep the merge uncommitted, and update '
            f'REPORT.md and STATUS in the meta directory.\n\n{failure["text"]}\n')


def job_text(cfg, job, wt, conflicts, commits, last_port):
    """The prompt's job section: a merge of new Windows commits, or a revisit of the backlog."""
    lines = [f'- You are in a disposable clone: `{wt}`, on branch `{job["port"]}`, which is based on '
             f'`{cfg["base_branch"]}`.']
    if job.get('revisit'):
        lines += [
            f'- This is a REVISIT run: there are no new Windows commits and no merge in progress (the steps about '
            f'conflicts do not apply). The branch already merges Windows `{job["branch"]}` up to `{job["target"][:10]}`.',
            '- Your task is the backlog: what earlier runs recorded as NOT PORTED (the "Not ported" sections of the '
            'integration records and the RE notes they link). Those runs worked statically only; you may run the '
            'game in the lab (see "Live testing"), which is how the open contracts are meant to be settled now. '
            'Port as many of the items as you can, most valuable first; each one you finish is progress even if '
            'the rest stay open, and each one that stays open needs a live attempt recorded, not a repeat of the '
            'static reasoning.',
        ]
        if job.get('focus'):
            lines += ['', 'The items, with what is known so far:', '', job['focus'].rstrip()]
    else:
        lines += [
            f'- A merge of the Windows branch `{job["branch"]}` at `{job["target"]}` is in progress and NOT committed. '
            f'It brings these {len(commits)} Windows commit(s) (see them with `git log -p HEAD..{job["target"]}`):',
            *[f'  - {c}' for c in commits],
            '- Merge conflicts to resolve: ' + (', '.join(conflicts) if conflicts else 'none'),
        ]
        if len(commits) < job['pending']:
            lines.append(f'- This run merges the oldest {len(commits)} of {job["pending"]} pending commits; the rest '
                         'follow in the next run.')
    if last_port == 'failed':
        lines.append('- The previous port run on this branch did NOT pass verification, so the tree may not build '
                     'yet. Make it pass as part of this job.')
    if job.get('base_note'):
        lines.append('- ' + job['base_note'])
    return '\n'.join(lines)


def render_prompt(cfg, rcfg, job, wt, meta, conflicts, commits, last_port):
    text = (HOME / 'prompt.md').read_text()
    verify_cmds = '; '.join('`' + ' '.join(c) + '`' for c in rcfg.get('verify', [])) or '(none configured)'
    values = {
        'JOB': job_text(cfg, job, wt, conflicts, commits, last_port),
        'REPO': job['repo'], 'BRANCH': job['branch'], 'PORT_BRANCH': job['port'], 'BASE_BRANCH': cfg['base_branch'],
        'TARGET': job['target'], 'COUNT': str(len(commits)), 'RANGE': f'HEAD..{job["target"]}',
        'COMMITS': '\n'.join(f'  - {c}' for c in commits), 'WORKDIR': str(wt), 'META': str(meta),
        'CONFLICTS': ', '.join(conflicts) if conflicts else 'none', 'VERIFY': verify_cmds,
        'DOCS': rcfg.get('docs', 'the docs/linux directory'),
        'PARTIAL': (f'This run merges the oldest {len(commits)} of {job["pending"]} pending commits; the rest follow '
                    'in the next run.' if len(commits) < job['pending'] else ''),
        'LAST_PORT': ('The previous port run on this branch did NOT pass verification, so the tree may not build yet. '
                      'Make it pass as part of this job.' if last_port == 'failed' else ''),
        'BASE_NOTE': job.get('base_note', ''),
    }
    for key, value in values.items():
        text = text.replace('{{' + key + '}}', value)
    return text


def publish_pr(cfg, repo, port, title, body_file, ok, comment_file=None):
    """Create the branch's pull request, or bring the existing one up to date: the
    description is rewritten (it describes the branch, not one job); a comment goes
    out only with comment_file, which run_job passes when the result needs a person
    (failed) or changed (recovered). Returns where it went."""
    mode = cfg['publish']
    slug = cfg['repos'][repo].get('github', f'{cfg["github_owner"]}/{repo}')
    base = cfg['base_branch']
    if mode == 'none':
        return 'not published (publish = none)'
    if mode == 'record':        # tests: a local stand-in for GitHub pull requests
        prs = load_json('prs.json', {})
        pr = prs.setdefault(slug, {}).get(port)
        event = {'slug': slug, 'head': port, 'base': base, 'title': title, 'draft': not ok,
                 'action': 'create' if pr is None else 'update', 'body': Path(body_file).read_text(),
                 'comment': Path(comment_file).read_text() if comment_file and pr is not None else None}
        prs[slug][port] = {'draft': not ok}
        save_json('prs.json', prs)
        with open(STATE / 'published.jsonl', 'a') as f:
            f.write(json.dumps(event) + '\n')
        return event['action']
    listing = subprocess.run(['gh', 'pr', 'list', '--repo', slug, '--head', port, '--base', base, '--state', 'open',
                              '--json', 'number,isDraft,url'], capture_output=True, text=True, check=True)
    existing = json.loads(listing.stdout or '[]')
    if not existing:
        cmd = ['gh', 'pr', 'create', '--repo', slug, '--base', base, '--head', port, '--title', title,
               '--body-file', str(body_file)] + ([] if ok else ['--draft'])
        return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip()
    pr = existing[0]
    # REST, not `gh pr edit`: gh 2.46 (Ubuntu) fails that with a projectCards deprecation error
    subprocess.run(['gh', 'api', '--method', 'PATCH', f'repos/{slug}/pulls/{pr["number"]}', '-f', f'title={title}',
                    '-F', f'body=@{body_file}'], capture_output=True, text=True, check=True)
    if comment_file:
        subprocess.run(['gh', 'pr', 'comment', str(pr['number']), '--repo', slug, '--body-file', str(comment_file)],
                       capture_output=True, text=True, check=True)
    if ok and pr['isDraft']:
        subprocess.run(['gh', 'pr', 'ready', str(pr['number']), '--repo', slug], capture_output=True, text=True, check=True)
    elif not ok and not pr['isDraft']:
        subprocess.run(['gh', 'pr', 'ready', '--undo', str(pr['number']), '--repo', slug],
                       capture_output=True, text=True, check=True)
    return pr['url']


def notify(summary):
    try:
        subprocess.run(['notify-send', '--app-name=tpf2-port', 'tpf2-port', summary], capture_output=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        pass


def compose_body(cfg, job, commits, conflicts, outcome, meta, total, runs=()):
    """The pull request's description: it stands for the whole port branch, so it
    carries every run so far (runs: oldest first, the current one last) and the
    latest job's details and agent report."""
    ok = outcome['ok']
    rcfg = cfg['repos'][job['repo']]
    ported = sum(r['commits'] for r in runs) or len(commits)
    lines = [
        f'Automated port of Windows branch `{job["branch"]}` into `{cfg["base_branch"]}`, run by tpf2-port on the Linux '
        'machine. One pull request per branch: each run adds its Windows commits and their port here; this '
        'description is rewritten after every run.', '',
        '| | |', '|---|---|',
        '| Latest result | ' + ('verification passed' if ok else '**verification failed: needs a person** (draft)') + ' |',
        f'| Ported so far | {ported} Windows commit(s) over {max(len(runs), 1)} run(s), up to `{job["target"][:10]}` |',
        '| This run | ' + (f'revisit of the backlog on top of `{job["target"][:10]}`' if job.get('revisit') else
                           f'{len(commits)} commit(s) merged at `{job["target"][:10]}`'
                           + (f' ({total - len(commits)} more follow in the next run)' if len(commits) < total else ''))
        + ' |',
        '| Merge conflicts | ' + (', '.join(f'`{c}`' for c in conflicts) if conflicts else 'none') + ' |',
        *([f'| Base | {job["base_note"]} |'] if job.get('base_note') else []),
        f'| Agent | {outcome["agent"] or "none"}' + (f', round {outcome["round"]}' if outcome['round'] else '')
        + (f', status {outcome["status"]}' if outcome['status'] else '') + ' |',
        '| Verification | ' + ('; '.join('`' + ' '.join(c) + '`' for c in rcfg.get('verify', [])) or 'none') + ' |',
        '', '<details><summary>Runs</summary>', '',
        '| When | Windows commits | Up to | Agent | Result |', '|---|---|---|---|---|',
        *[f'| {r["at"][:16].replace("T", " ")} | {r["commits"]}{" (revisit)" if r.get("kind") == "revisit" else ""} | '
          f'`{r["target"][:10]}` | {r["agent"] or "none"} | {"passed" if r["ok"] else "FAILED"} |' for r in runs],
        '', '</details>', '',
        '<details><summary>Windows commits in this run</summary>', '', *[f'- {c}' for c in commits], '', '</details>', '',
        '<details><summary>Rounds</summary>', '', *[f'- {h}' for h in outcome['history']], '', '</details>', '',
        '## Agent report (this run)', '',
    ]
    report = tail(meta / 'REPORT.md', 400)
    lines.append(report if report else '_The agent wrote no report._')
    if not ok and outcome.get('failure'):
        lines += ['', '## Why it stopped', '', outcome['failure']['text']]
    body = '\n'.join(lines)
    if len(body) > 60000:
        body = body[:60000] + '\n\n_(truncated; the full logs are in the job directory on the Linux machine)_'
    return body


def run_job(cfg, job, publish=True):
    repo, branch, port = job['repo'], job['branch'], job['port']
    rcfg = cfg['repos'][repo]
    base = cfg['base_branch']
    safe = ''.join(ch if ch.isalnum() or ch in '._-' else '_' for ch in branch)
    stem = f'{repo}-{safe}-{"revisit" if job.get("revisit") else job["tip"][:10]}-{datetime.now():%Y%m%d-%H%M%S}'
    JOBS.mkdir(parents=True, exist_ok=True)
    for n in range(1, 1000):        # a long branch runs several jobs on the same tip, possibly within a second
        jd = JOBS / (stem if n == 1 else f'{stem}-{n}')
        try:
            jd.mkdir()
            break
        except FileExistsError:
            continue
    wt, meta = jd / 'repo', jd / 'meta'
    meta.mkdir()
    log(f'{repo} {branch}: ' + ('revisit of the backlog' if job.get('revisit') else f'{job["pending"]} commit(s) to port')
        + f' -> {jd}')

    git(HOME, 'clone', '--quiet', '--shared', str(mirror_path(repo)), str(wt))
    git(wt, 'fetch', '--quiet', 'origin', f'+refs/windows/{branch}:refs/windows/{branch}')
    remote = refs_of(wt, 'refs/remotes/origin')
    if job.get('revisit') and port not in remote:
        raise GitError(f'{port} does not exist on GitHub yet: nothing to revisit')
    git(wt, 'checkout', '--quiet', '-B', port, f'origin/{port}' if port in remote else f'origin/{base}')
    git(wt, 'config', 'user.name', cfg['git_name'])
    git(wt, 'config', 'user.email', cfg['git_email'])

    # An existing port branch first catches up with linux-native, so the port is made
    # on the current Linux tree. A conflicting catch-up is left for the pull request.
    base_note = ''
    if port in remote and subprocess.run(['git', '-C', str(wt), 'merge-base', '--is-ancestor', f'origin/{base}',
                                          'HEAD']).returncode != 0:
        up = subprocess.run(['git', '-C', str(wt), 'merge', '--no-ff', '-m', f'Merge {base} into {port}', f'origin/{base}'],
                            capture_output=True, text=True)
        if up.returncode != 0:
            subprocess.run(['git', '-C', str(wt), 'merge', '--abort'], capture_output=True)
            base_note = (f'`{port}` conflicts with the current `{base}`, so this port is made on the older base; '
                         'the conflict is resolved when the pull request is merged.')
    job['base_note'] = base_note

    if job.get('revisit'):
        job['target'] = git(wt, 'rev-parse', 'HEAD')      # nothing to merge: the tree as it stands
        pending, commits, conflicts = [], [], []
    else:
        pending = git(wt, 'rev-list', '--reverse', '--topo-order', f'refs/windows/{branch}', '^HEAD',
                      f'^origin/{base}').split()
        if not pending:
            log(f'{repo} {branch}: nothing left to port')
            shutil.rmtree(jd)
            return
        job['target'] = pending[-1] if len(pending) <= cfg['max_commits'] else pending[cfg['max_commits'] - 1]
        commits = git(wt, 'log', '--reverse', '--no-decorate', '--format=%h %s', job['target'], '^HEAD',
                      f'^origin/{base}').splitlines()
        merge = subprocess.run(['git', '-C', str(wt), 'merge', '--no-ff', '--no-commit', job['target']],
                               capture_output=True, text=True)
        conflicts = git(wt, 'diff', '--name-only', '--diff-filter=U').split()
        if merge.returncode != 0 and not conflicts:
            raise GitError(f'merge of {job["target"]} failed: {(merge.stderr or merge.stdout).strip()}')
    (meta / 'job.json').write_text(json.dumps({**job, 'commits': commits, 'conflicts': conflicts}, indent=2))

    last_port = load_json('ports.json', {}).get(repo, {}).get(branch, {}).get('result')
    prompt = render_prompt(cfg, rcfg, job, wt, meta, conflicts, commits, last_port if port in remote else None)
    (meta / 'prompt.md').write_text(prompt)
    outcome = run_agents(cfg, rcfg, job, wt, meta, prompt)
    ok = outcome['ok']

    git(wt, 'add', '-A')
    merging = git(wt, 'rev-parse', '-q', '--verify', 'MERGE_HEAD', check=False) != ''
    staged = subprocess.run(['git', '-C', str(wt), 'diff', '--cached', '--quiet']).returncode != 0
    if job.get('revisit'):
        message = (f'Revisit the Linux port of {branch} at {job["target"][:7]}\n\n'
                   f'Backlog run: no new Windows commits; ports what earlier runs left unported, with live testing.\n'
                   f'Verification: {"passed" if ok else "FAILED"}. Agent: {outcome["agent"] or "none"}.\n')
    else:
        message = (f'Port Windows {branch} {job["target"][:7]} to Linux\n\n'
                   f'Merges {len(commits)} Windows commit(s) from {branch} and ports them to the native Linux build.\n'
                   f'Verification: {"passed" if ok else "FAILED"}. Agent: {outcome["agent"] or "none"}.\n')
    if merging or staged:
        (meta / 'COMMIT_MSG').write_text(message)
        git(wt, 'commit', '--quiet', '--no-verify', '-F', str(meta / 'COMMIT_MSG'))

    ports = load_json('ports.json', {})
    entry = ports.setdefault(repo, {}).setdefault(branch, {})
    previous = entry.get('result')
    run = {'at': datetime.now().isoformat(timespec='minutes'), 'target': job['target'], 'commits': len(commits),
           'agent': outcome['agent'], 'ok': ok, 'job': jd.name, 'kind': 'revisit' if job.get('revisit') else 'port'}
    runs = (entry.get('runs') or [])[-59:] + [run]
    body = compose_body(cfg, job, commits, conflicts, outcome, meta, len(pending), runs)
    (meta / 'PR_BODY.md').write_text(body)
    title = f'Port Windows {branch} to Linux'
    result = 'passed' if ok else 'failed'
    # A comment (a notification) only when a person is needed or the branch recovered.
    comment_file = None
    if not ok or previous == 'failed':
        text = (f'Run {run["at"].replace("T", " ")}: verification ' + ('passed again' if ok else '**FAILED**')
                + f' -- {len(commits)} Windows commit(s) up to `{job["target"][:10]}`, agent {outcome["agent"] or "none"}'
                + ('' if ok else '; the branch is a draft until a person or a later run fixes it')
                + '. The description above has the report.')
        if not ok and outcome.get('failure'):
            text += '\n\n' + outcome['failure']['text'][-3000:]
        (meta / 'PR_COMMENT.md').write_text(text)
        comment_file = meta / 'PR_COMMENT.md'
    if publish:
        push = subprocess.run(['git', '-C', str(wt), '-c', 'credential.helper=', '-c', 'credential.helper=!gh auth git-credential',
                               'push', '--quiet', github_url(cfg, repo), f'HEAD:refs/heads/{port}'],
                              capture_output=True, text=True)
        if push.returncode != 0:
            raise GitError(f'push of {port} failed: {(push.stderr or push.stdout).strip()}')
        git(mirror_path(repo), 'fetch', '--quiet', '--prune', 'origin')
        where = publish_pr(cfg, repo, port, title, meta / 'PR_BODY.md', ok, comment_file)
        ports = load_json('ports.json', {})
        ports.setdefault(repo, {})[branch] = {'result': result, 'target': job['target'], 'at': datetime.now().isoformat(),
                                              'runs': runs}
        save_json('ports.json', ports)
    else:
        where = 'not published (--no-publish)'
    attempts = load_json('attempts.json', {})
    attempts.get(repo, {}).pop(branch, None)
    save_json('attempts.json', attempts)
    summary = f'{repo} {branch}: port {result} ({outcome["agent"] or "no agent"}); {where}'
    log(summary)
    notify(summary)


def session_environment():
    """Started by a git hook, the worker has no desktop environment. The agents run the
    game in the lab (2026-09-17), which needs the user's session: take DISPLAY and its
    companions from systemd's user environment when they are not set."""
    try:
        out = subprocess.run(['systemctl', '--user', 'show-environment'], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return
    for line in out.stdout.splitlines():
        key, sep, value = line.partition('=')
        if sep and key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR', 'XAUTHORITY', 'XDG_SESSION_TYPE',
                           'DBUS_SESSION_BUS_ADDRESS') and key not in os.environ:
            os.environ[key] = value


def note_failure(job, err):
    attempts = load_json('attempts.json', {})
    entry = attempts.setdefault(job['repo'], {}).get(job['branch'])
    if not entry or entry['tip'] != job['tip']:
        entry = {'tip': job['tip'], 'failures': 0}
    entry['failures'] += 1
    entry['error'] = str(err)[:2000]
    attempts[job['repo']][job['branch']] = entry
    save_json('attempts.json', attempts)
    notify(f'{job["repo"]} {job["branch"]}: port job error: {str(err)[:200]}')


def prune_jobs(cfg):
    if not JOBS.exists():
        return
    dirs = sorted((d for d in JOBS.iterdir() if d.is_dir()), key=lambda d: d.stat().st_mtime, reverse=True)
    for d in dirs[cfg['keep_jobs']:]:
        shutil.rmtree(d, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--plan', action='store_true')
    ap.add_argument('--baseline', action='store_true')
    ap.add_argument('--once', action='store_true')
    ap.add_argument('--no-publish', action='store_true')
    ap.add_argument('--revisit', metavar='REPO', help='run a backlog job for REPO (see the module docstring)')
    ap.add_argument('--branch', default='dev', help='the Windows branch whose port branch a --revisit works on')
    ap.add_argument('--focus', metavar='FILE', help='--revisit: a Markdown list of the items to work on')
    args = ap.parse_args()
    cfg = load_config()
    # Started by the inbox's post-receive hook, the worker inherits GIT_DIR=. (and the
    # quarantine paths) from git; every git call below, and the agents' own, would
    # then address the wrong repository.
    for key in [k for k in os.environ if k.startswith('GIT_') and k != 'GIT_SSH_COMMAND']:
        del os.environ[key]
    os.environ['PATH'] =f'{Path.home() / ".local/bin"}:{os.environ.get("PATH", "")}'
    bus = Path(f'/run/user/{os.getuid()}/bus')
    if 'DBUS_SESSION_BUS_ADDRESS' not in os.environ and bus.exists():
        os.environ['DBUS_SESSION_BUS_ADDRESS'] = f'unix:path={bus}'   # gh's keyring and notify-send
    session_environment()
    STATE.mkdir(parents=True, exist_ok=True)

    if args.revisit:
        if args.revisit not in cfg['repos']:
            ap.error(f'{args.revisit} is not in config.json')
        focus = Path(args.focus).read_text() if args.focus else ''
        lock = open(STATE / 'worker.lock', 'w')
        log(f'{args.revisit} {args.branch}: revisit requested; waiting for the worker lock')
        fcntl.flock(lock, fcntl.LOCK_EX)          # after the running job, if any
        refresh(cfg, args.revisit)
        tips = refs_of(inbox_path(args.revisit), 'refs/heads')
        job = {'repo': args.revisit, 'branch': args.branch, 'tip': tips.get(args.branch, 'revisit'),
               'port': port_branch(args.branch), 'pending': 0, 'when': int(time.time()), 'revisit': True,
               'focus': focus}
        try:
            run_job(cfg, job, publish=not args.no_publish)
        except Exception as err:
            log(f'{args.revisit} {args.branch}: revisit error: {err}\n{traceback.format_exc()}')
            return 1
        finally:
            prune_jobs(cfg)
        return 0

    if args.baseline:
        record_baseline(cfg)
        return 0
    if args.plan:
        for j in plan(cfg):
            print(f'{j["repo"]:20} {j["branch"]:40} {j["pending"]:4} commit(s) -> {j["port"]}')
        return 0

    lock = open(STATE / 'worker.lock', 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return 0                     # the running worker re-plans after every job
    snapshot = None
    while True:
        snapshot = inbox_snapshot(cfg)
        try:
            jobs = plan(cfg)
        except GitError as err:
            log(f'planning failed (offline?): {err}')
            break
        if not jobs:
            break
        try:
            run_job(cfg, jobs[0], publish=not args.no_publish)
        except Exception as err:     # one bad job must not stop the queue
            log(f'{jobs[0]["repo"]} {jobs[0]["branch"]}: job error: {err}\n{traceback.format_exc()}')
            note_failure(jobs[0], err)
        prune_jobs(cfg)
        if args.once:
            return 0
    fcntl.flock(lock, fcntl.LOCK_UN)
    lock.close()
    if snapshot is not None and inbox_snapshot(cfg) != snapshot:
        os.execv(sys.executable, [sys.executable, *sys.argv])   # a push landed while this worker held the lock
    return 0


if __name__ == '__main__':
    sys.exit(main())
