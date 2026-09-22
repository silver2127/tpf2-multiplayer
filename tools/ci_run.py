#!/usr/bin/env python3
"""Wait for the GitHub Actions run of a commit and print each step's result;
on failure, print the error lines of the job log.

    python tools/ci_run.py <sha-prefix> [--repo silver2127/tpf2-multiplayer] [--timeout 1800]

The log download needs a token: the stored git credential for github.com is
used (git credential fill), sent only to api.github.com; the signed storage
URL it redirects to is fetched without it.
"""
import argparse, json, re, subprocess, sys, time, urllib.request, urllib.error

def api(url, tok=None, follow=True):
    h = {'Accept': 'application/vnd.github+json', 'User-Agent': 'tpf2-ci', 'X-GitHub-Api-Version': '2022-11-28'}
    if tok: h['Authorization'] = 'token ' + tok
    req = urllib.request.Request(url, headers=h)
    if follow: return urllib.request.urlopen(req).read().decode('utf-8', 'ignore')
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *a, **k): return None
    try:
        return urllib.request.build_opener(NoRedirect).open(req).read().decode('utf-8', 'ignore')
    except urllib.error.HTTPError as e:
        if e.code in (301, 302, 307) and e.headers.get('Location'):
            return urllib.request.urlopen(urllib.request.Request(e.headers['Location'], headers={'User-Agent': 'tpf2-ci'})).read().decode('utf-8', 'ignore')
        raise

def token():
    out = subprocess.run(['git', 'credential', 'fill'], input='protocol=https\nhost=github.com\n', capture_output=True, text=True).stdout
    return dict(l.split('=', 1) for l in out.strip().splitlines() if '=' in l).get('password', '')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sha'); ap.add_argument('--repo', default='silver2127/tpf2-multiplayer'); ap.add_argument('--timeout', type=int, default=1800)
    a = ap.parse_args()
    base = f'https://api.github.com/repos/{a.repo}/actions'
    t0 = time.time(); run = None
    while time.time() - t0 < a.timeout:
        runs = json.loads(api(base + '/runs?per_page=10'))['workflow_runs']
        run = next((r for r in runs if r['head_sha'].startswith(a.sha)), None)
        if run and run['status'] == 'completed': break
        print('...', run['status'] if run else 'no run yet', flush=True); time.sleep(20)
    if not run: print('no run found'); return 2
    print(run['conclusion'], run['html_url'])
    jobs = json.loads(api(base + f"/runs/{run['id']}/jobs"))['jobs']
    for j in jobs:
        for s in j['steps']: print(f"  {s['conclusion'] or s['status']:9} {s['name']}")
    if run['conclusion'] == 'success': return 0
    tok = token()
    for j in jobs:
        if j['conclusion'] != 'failure': continue
        log = api(base + f"/jobs/{j['id']}/logs", tok, follow=False).splitlines()
        hits = [l for l in log if re.search(r'(?i)error|fail|LNK\d|fatal|not recognized|not found|missing|cannot', l) and 'setup-python' not in l]
        print('--- error lines'); print('\n'.join(l[:220] for l in hits[-40:]))
    return 1

if __name__ == '__main__': sys.exit(main())
