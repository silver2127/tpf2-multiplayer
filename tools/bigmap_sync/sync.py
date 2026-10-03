#!/usr/bin/env python3
"""Mirror bigmap/ into the standalone tpf2-bigmap repository.

bigmap/ here is the one source of Big Maps. tpf2-bigmap stays a standalone
download, so every commit on dev that touches Big Maps is replayed there as one
commit (same author, date and message, plus a Synced-from trailer). Each
replayed tree is a pure function of the multiplayer commit and the standalone
repo's own files, so a rerun makes the same commits and a sync that is already
up to date makes none.

The standalone tree at multiplayer commit C:
  * every file of C:bigmap/, at the repository root;
  * except the standalone-owned paths (OWNED), which keep the standalone repo's
    own content: its installer, CI and MSI tooling live only there;
  * src/tpf2mp_plugin.h = C:native/src/plugin/tpf2mp_plugin.h (the plugin ABI,
    which the standalone build cannot reach any other way), and includes of
    ../../native/src/plugin/tpf2mp_plugin.h point at that copy;
  * README.md: each <!-- standalone:NAME --> ... <!-- /standalone:NAME --> block
    is replaced by C:tools/bigmap_sync/readme/NAME.md.

Usage (both checkouts must be full clones; the standalone one on its dev branch):
  python tools/bigmap_sync/sync.py --mp . --sa ../tpf2-bigmap [--rev origin/dev]
      [--base <sha>] [--squash] [--push] [--dry-run]

--base names the last multiplayer commit already mirrored, for the first run
only; later runs read it from the newest Synced-from trailer. --squash replays
the whole backlog as one commit (used once, for the first sync).
"""
import argparse, os, posixpath, re, subprocess, sys, tempfile

OWNED = (".github/", "installer/", "tools/vendor_host.ps1", "tools/test_config_msi.py")
ABI_SRC = "native/src/plugin/tpf2mp_plugin.h"
ABI_DST = "src/tpf2mp_plugin.h"
ABI_INCLUDE = "../../native/src/plugin/tpf2mp_plugin.h"
WATCH = ("bigmap", ABI_SRC, "tools/bigmap_sync")
TRAILER = "Synced-from: silver2127/tpf2-multiplayer@"
BLOCK = re.compile(r"<!-- standalone:([a-z0-9-]+) -->\n.*?<!-- /standalone:\1 -->\n", re.S)
# build.bat's header names the multiplayer layout; the standalone one says what it has.
BUILD_BAT_HEADER = (
    re.compile(rb"REM tpf2_bigmap\.dll -- a tpf2mp plugin\. Its one dependency on the rest of the(\r?\n)"
               rb"REM repo is the plugin ABI, native\\src\\plugin\\tpf2mp_plugin\.h\.\r?\n"
               rb"REM native\\build\.bat bigmap \(and all\) call this script\.\r?\n"),
    lambda m: (b"REM tpf2_bigmap.dll -- a tpf2mp plugin. Standalone: nothing here depends on the" + m.group(1) +
               b"REM host's source tree, only on the vendored src\\tpf2mp_plugin.h." + m.group(1)),
)


def git(repo, *args, input=None, env=None):
    r = subprocess.run(["git", "-C", repo, *args], input=input, capture_output=True,
                       env={**os.environ, **(env or {})})
    if r.returncode:
        sys.exit(f"git {' '.join(args)} failed in {repo}:\n{r.stderr.decode(errors='replace')}")
    return r.stdout


def owned(path):
    return any(path == o or (o.endswith("/") and path.startswith(o)) for o in OWNED)


def ls_tree(repo, rev, prefix=None):
    """{path: (mode, sha)} of blobs (and gitlinks) under prefix, relative to it."""
    out = {}
    args = ["ls-tree", "-r", "-z", "--full-tree", rev] + ([prefix] if prefix else [])
    for rec in git(repo, *args).split(b"\0"):
        if not rec:
            continue
        meta, path = rec.split(b"\t", 1)
        mode, _typ, sha = meta.decode().split()
        path = path.decode("utf-8")
        if prefix:
            path = path[len(prefix) + 1:]
        out[path] = (mode, sha)
    return out


def blob(repo, sha):
    return git(repo, "cat-file", "blob", sha)


def transform(path, data, readme_frags):
    if path == "README.md":
        text = data.decode("utf-8")
        missing = []

        def sub(m):
            frag = readme_frags.get(m.group(1))
            if frag is None:
                missing.append(m.group(1))
                return m.group(0)
            return frag
        text = BLOCK.sub(sub, text)
        if missing:
            sys.exit(f"README blocks without a tools/bigmap_sync/readme fragment: {missing}")
        return text.encode("utf-8")
    if path == "build.bat":
        return BUILD_BAT_HEADER[0].sub(BUILD_BAT_HEADER[1], data, count=1)
    if ABI_INCLUDE.encode() in data:
        rel = posixpath.relpath(ABI_DST, posixpath.dirname(path) or ".")
        return data.replace(ABI_INCLUDE.encode(), rel.encode())
    return data


def mirror_tree(mp, sa, rev, sa_parent):
    """Write the standalone tree for multiplayer commit rev into sa's object store; return its sha."""
    files = ls_tree(mp, rev, "bigmap")
    if not files:
        sys.exit(f"{rev} has no bigmap/")
    frags = {}
    for p, (_m, sha) in ls_tree(mp, rev, "tools/bigmap_sync/readme").items():
        if p.endswith(".md"):
            frags[p[:-3]] = blob(mp, sha).decode("utf-8")
    entries = {}
    with tempfile.TemporaryDirectory() as tmp:
        pending = []
        abi = ls_tree(mp, rev, ABI_SRC.rsplit("/", 1)[0]).get(ABI_SRC.rsplit("/", 1)[1])
        if abi is None:
            sys.exit(f"{rev} has no {ABI_SRC}")
        for path, (mode, sha) in list(files.items()) + [(ABI_DST, abi)]:
            if owned(path):
                continue
            data = blob(mp, sha)
            data = transform(path, data, frags)
            f = os.path.join(tmp, str(len(pending)))
            with open(f, "wb") as fh:
                fh.write(data)
            pending.append((path, mode, f))
        shas = git(sa, "hash-object", "-w", "--no-filters", "--stdin-paths",
                   input="\n".join(f for _, _, f in pending).encode()).decode().split()
        for (path, mode, _f), sha in zip(pending, shas):
            entries[path] = (mode, sha)
    if sa_parent:
        for path, (mode, sha) in ls_tree(sa, sa_parent).items():
            if owned(path):
                entries[path] = (mode, sha)
    index = os.path.join(git(sa, "rev-parse", "--absolute-git-dir").decode().strip(), "bigmap-sync.index")
    env = {"GIT_INDEX_FILE": index}
    try:
        git(sa, "read-tree", "--empty", env=env)
        info = "".join(f"{m} {s}\t{p}\n" for p, (m, s) in sorted(entries.items()))
        git(sa, "update-index", "--index-info", input=info.encode("utf-8"), env=env)
        return git(sa, "write-tree", env=env).decode().strip()
    finally:
        if os.path.exists(index):
            os.remove(index)


def commit_meta(mp, rev):
    fmt = "%an%x00%ae%x00%ad%x00%cn%x00%ce%x00%cd%x00%B"
    an, ae, ad, cn, ce, cd, body = git(mp, "show", "-s", "--date=raw", f"--format={fmt}", rev).decode(
        "utf-8").split("\0", 6)
    return {"GIT_AUTHOR_NAME": an, "GIT_AUTHOR_EMAIL": ae, "GIT_AUTHOR_DATE": ad,
            "GIT_COMMITTER_NAME": cn, "GIT_COMMITTER_EMAIL": ce, "GIT_COMMITTER_DATE": cd}, body.rstrip()


def last_synced(sa, head):
    log = git(sa, "log", "-n", "500", "--format=%B%x00", head).decode("utf-8")
    m = re.search(re.escape(TRAILER) + r"([0-9a-f]{40})", log)
    return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mp", default=".")
    ap.add_argument("--sa", required=True)
    ap.add_argument("--rev", default="origin/dev")
    ap.add_argument("--branch", default="dev", help="standalone branch to extend and push")
    ap.add_argument("--base")
    ap.add_argument("--squash", action="store_true")
    ap.add_argument("--push", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    rev = git(a.mp, "rev-parse", a.rev + "^{commit}").decode().strip()
    head = git(a.sa, "rev-parse", "HEAD").decode().strip()
    base = last_synced(a.sa, head) or a.base
    if not base:
        sys.exit("no Synced-from trailer in the standalone history: pass --base <last mirrored commit>")
    base = git(a.mp, "rev-parse", base + "^{commit}").decode().strip()
    if subprocess.run(["git", "-C", a.mp, "merge-base", "--is-ancestor", base, rev]).returncode:
        sys.exit(f"{base[:10]} is not an ancestor of {rev[:10]}: dev was rewritten, sync by hand")
    todo = git(a.mp, "rev-list", "--reverse", "--first-parent", f"{base}..{rev}", "--", *WATCH).decode().split()
    print(f"mirrored up to {base[:10]}; {len(todo)} commit(s) to replay up to {rev[:10]}")
    if not todo:
        return
    if a.squash:
        todo = [todo[-1]]

    parent = head
    made = 0
    for c in todo:
        tree = mirror_tree(a.mp, a.sa, c, parent)
        env, body = commit_meta(a.mp, c)
        if a.squash:
            subjects = git(a.mp, "log", "--first-parent", "--format=  %h %s", f"{base}..{c}", "--",
                           *WATCH).decode("utf-8").rstrip()
            body = (f"Sync bigmap/ from tpf2-multiplayer up to {c[:10]}\n\n"
                    f"Big Maps is developed under bigmap/ in tpf2-multiplayer. This commit brings\n"
                    f"this repository up to its dev branch; later commits there are replayed here\n"
                    f"one by one. Covers:\n{subjects}")
        if git(a.sa, "rev-parse", parent + "^{tree}").decode().strip() == tree:
            print(f"  {c[:10]} no change to the standalone tree")
            continue
        msg = f"{body}\n\n{TRAILER}{c}\n"
        parent = git(a.sa, "commit-tree", tree, "-p", parent, "-F", "-", input=msg.encode("utf-8"),
                     env=env).decode().strip()
        made += 1
        print(f"  {c[:10]} -> {parent[:10]} {body.splitlines()[0][:70]}")
    if not made:
        return
    if a.dry_run:
        print(f"dry run: new head would be {parent} (not moved, not pushed)")
        return
    git(a.sa, "update-ref", f"refs/heads/{a.branch}", parent, head)
    if git(a.sa, "symbolic-ref", "-q", "HEAD").decode().strip() == f"refs/heads/{a.branch}":
        git(a.sa, "reset", "-q", "--hard", parent)
    if a.push:
        git(a.sa, "push", "origin", f"{parent}:refs/heads/{a.branch}")
        print(f"pushed {parent[:10]} to {a.branch}")


if __name__ == "__main__":
    main()
