#!/usr/bin/env python3
"""Complete a release: the two launchers on the version's page v<version>, its
install files to the packages repository and to the release <version>, then publish.

    python tools/publish_release.py v0.7.0.6 --linux-dir DIR [--publish]
    python tools/publish_release.py launcher [--publish]     a launcher update on its own
    python tools/publish_release.py page v0.7.0.5 [--publish] a published version to this layout

THE LAYOUT (2026-09-26, the user: releases should "only show 2 things to download
the linux launcher and the windows launcher"; "move to the two launcher model with
another tagged release for the update files"; "two launcher on the original page",
and, asked, launchers on top even though launchers up to 1.2.0 then cannot install):

    v0.7.0.6   the version's page, first in the list and marked Latest: exactly
               TpF2Multiplayer-Launcher-Windows-Setup.exe   tearded's Setup.exe
               TpF2Multiplayer-Launcher-Linux.AppImage      its Linux launcher
               under names that do not change (.../releases/latest/download/<name>),
               and the direct installers (the user, 2026-09-26: "in addition to the
               launcher download the install msi, proton and native linux.run"):
               TpF2Multiplayer.msi  install_proton.sh  tpf2mp-linux-<v>-native.run
               TpF2Multiplayer-Server-Linux.tar.gz (the dedicated server's scripts)
               -- which also lets launchers up to 1.2.0 install it again: they look
               for TpF2Multiplayer.msi on v<version>
    0.7.0.6    the update files, tagged without the "v", not Latest:
               TpF2Multiplayer.msi  TpF2Multiplayer-files.zip  install_proton.py
               install_proton.sh  SHA256SUMS.txt  tpf2mp-linux-<v>-native.run/.tar.gz/.sha256
               TpF2Multiplayer-Server-Linux.tar.gz
    silver2127/tpf2-multiplayer-packages v0.7.0.6   the same install files

Launchers from 1.3.0 skip a tag without the "v" whose v twin is listed (and
launcher-v* releases), and install from the packages release with the v tag; so
do the Proton script and the Linux launcher. Launchers up to 1.2.0 look for the MSI
on v<version> and cannot install a version published this way: their players
update the launcher first (Settings). The releases list is ordered by GitHub in a
way publication dates do not decide (2026-09-26: a page tagged without the "v" and
published last still listed below v0.7.0.4), so the launchers are on the v tag.
Only v* tags start the release workflow.

LAUNCHER RELEASES (the user, 2026-09-26: "make separate releases for the launcher
updates, tag them as something else"). `launcher` makes a release tagged
launcher-v<launcher version> with the two files under the same names, published
WITHOUT the "Latest" mark.

WHAT IT DOES for a version, in order (each step checks, nothing is published half-done):
  1. the DRAFT release v<version> (the Build MSI workflow makes it with the notes;
     see .github/workflows/build-msi.yml);
  2. the install files: the tag run's workflow artifact (or --payload-dir), checked
     against its SHA256SUMS.txt; the native Linux package from --linux-dir
     (tools/linux/build_release.sh on strelka), checked against its .sha256;
  3. the packages release for the tag (created if missing, pre-release like the mod
     release), and each file uploaded unless the same bytes are already there;
  4. the launchers: tearded's newest launcher release (or --windows-launcher /
     --linux-launcher files), checked against that release's SHA256SUMS.txt;
  5. v<version> with exactly the two launchers; the release <version> (a draft,
     created if missing) with exactly the install files;
  6. with --publish: <version> (never Latest), then v<version> (Latest unless a
     pre-release).
Without a Linux launcher yet, --no-linux-launcher publishes the Windows one alone.

PAGE: a version published with its files on v<version> (0.7.0.5 and older) gets
this layout: the files copied to the packages release and to <version>, checked,
then the launchers onto v<version> and its files removed from it.

The token is GH_TOKEN, else the one Git stores for github.com (git credential fill).
"""
import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO = "silver2127/tpf2-multiplayer"
PACKAGES_REPO = "silver2127/tpf2-multiplayer-packages"
LAUNCHER_REPO = "tearded/tpf-multiplayer-launcher"
WINDOWS_NAME = "TpF2Multiplayer-Launcher-Windows-Setup.exe"
LINUX_NAME = "TpF2Multiplayer-Launcher-Linux.AppImage"
PAYLOAD = ["TpF2Multiplayer.msi", "TpF2Multiplayer-files.zip", "TpF2Multiplayer-Server-Linux.tar.gz", "install_proton.py", "install_proton.sh", "SHA256SUMS.txt"]
API = "https://api.github.com"
# the dedicated server's scripts (tools/server/build_package.py), under a name that does not change
SERVER_NAME = "TpF2Multiplayer-Server-Linux.tar.gz"


def say(text):
    print(text, flush=True)


def fail(text):
    raise SystemExit(f"error: {text}")


def token():
    t = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if t:
        return t
    out = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                         capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        if line.startswith("password="):
            return line[9:]
    fail("no GitHub token: set GH_TOKEN or store one with git")


class GitHub:
    def __init__(self, tok, dry):
        self.tok, self.dry = tok, dry

    def call(self, method, url, body=None, data=None, ctype=None, raw=False):
        if not url.startswith("http"):
            url = API + url
        headers = {"Authorization": f"Bearer {self.tok}", "Accept": "application/vnd.github+json",
                   "User-Agent": "tpf2mp-publish-release"}
        if body is not None:
            data, ctype = json.dumps(body).encode(), "application/json"
        if ctype:
            headers["Content-Type"] = ctype
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        with urllib.request.urlopen(req, timeout=600) as r:
            payload = r.read()
        return payload if raw else (json.loads(payload) if payload else None)

    def redirected(self, url):
        """An artifact: the API answers 302 to a signed storage URL, which refuses our
        token (urllib would forward it), so the redirect is fetched without it."""
        class Stop(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *a, **k):
                return None
        req = urllib.request.Request(url, headers={"Authorization": f"Bearer {self.tok}",
                                                   "User-Agent": "tpf2mp-publish-release"})
        try:
            urllib.request.build_opener(Stop).open(req, timeout=60)
            fail(f"no redirect for {url}")
        except urllib.error.HTTPError as e:
            if e.code not in (301, 302, 303, 307, 308):
                raise
            target = e.headers["Location"]
        with urllib.request.urlopen(target, timeout=600) as r:
            return r.read()

    def get(self, path):
        try:
            return self.call("GET", path)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise

    def write(self, what, method, url, **kw):
        if self.dry:
            say(f"  (dry run) would {what}")
            return None
        return self.call(method, url, **kw)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def sums(text):
    """'<hash>  <name>' lines -> {name: hash}."""
    out = {}
    for line in text.replace("\r", "").splitlines():
        parts = line.split()
        if len(parts) >= 2 and re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
            out[parts[-1].lstrip("*")] = parts[0].lower()
    return out


def draft_release(gh, tag):
    for page in range(1, 6):
        for r in gh.call("GET", f"/repos/{REPO}/releases?per_page=100&page={page}"):
            if r["tag_name"] == tag:
                return r
    fail(f"{REPO} has no release for {tag}: push the tag and let the Build MSI workflow draft it first")


def find_release(gh, tag):
    """The release for a tag, drafts included (releases/tags/<tag> does not see drafts)."""
    for page in range(1, 6):
        batch = gh.call("GET", f"/repos/{REPO}/releases?per_page=100&page={page}")
        for r in batch:
            if r["tag_name"] == tag:
                return r
        if len(batch) < 100:
            return None
    return None


def tag_commit(gh, tag):
    ref = gh.call("GET", f"/repos/{REPO}/git/ref/tags/{tag}")["object"]
    return gh.call("GET", f"/repos/{REPO}/git/tags/{ref['sha']}")["object"]["sha"] if ref["type"] == "tag" else ref["sha"]


def published(gh, rel, latest):
    # tag_name travels with the publish: v0.7.0.7's page came out of a bare
    # {"draft": false} as "untagged-<hash>" (2026-09-27), its tag in place but not
    # attached, and needed a second PATCH with tag_name
    change = {"draft": False, "make_latest": "true" if latest else "false", "tag_name": rel["tag_name"]}
    gh.write(f"publish {rel['tag_name']}{'' if latest else ' (not Latest)'}", "PATCH",
             f"/repos/{REPO}/releases/{rel['id']}", body=change)
    if not getattr(gh, "dry", True):
        now = gh.call("GET", f"/repos/{REPO}/releases/{rel['id']}")
        if now["tag_name"] != rel["tag_name"]:
            fail(f"published, but GitHub reports its tag as {now['tag_name']!r}, not {rel['tag_name']!r}: "
                 f"PATCH /releases/{rel['id']} with tag_name {rel['tag_name']!r}")


def payload_from_ci(gh, tag, into):
    runs = gh.call("GET", f"/repos/{REPO}/actions/runs?event=push&per_page=50")["workflow_runs"]
    run = next((r for r in runs if r["head_branch"] == tag and r["name"] == "Build MSI"), None)
    if run is None:
        fail(f"no Build MSI run for {tag}")
    if run["conclusion"] != "success":
        fail(f"the Build MSI run for {tag} is {run['status']}/{run['conclusion']}: wait for it, or pass --payload-dir")
    arts = gh.call("GET", f"/repos/{REPO}/actions/runs/{run['id']}/artifacts")["artifacts"]
    art = next((a for a in arts if a["name"].startswith("TpF2Multiplayer-") and not a["expired"]), None)
    if art is None:
        fail(f"the {tag} run has no TpF2Multiplayer-* artifact (expired?): pass --payload-dir")
    say(f"payload: artifact {art['name']} of run {run['id']}")
    blob = gh.redirected(art["archive_download_url"])
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        for name in PAYLOAD:
            (into / name).write_bytes(z.read(name))
    return into


def check_payload(folder, version):
    missing = [n for n in PAYLOAD if not (folder / n).is_file()]
    if missing:
        fail(f"the payload lacks {', '.join(missing)}")
    listed = sums((folder / "SHA256SUMS.txt").read_text())
    for name in ("TpF2Multiplayer.msi", "TpF2Multiplayer-files.zip", "TpF2Multiplayer-Server-Linux.tar.gz"):
        if listed.get(name) != sha256(folder / name):
            fail(f"{name} does not match SHA256SUMS.txt")
    for script, stamp in (("install_proton.py", f'DEFAULT_VERSION = "{version}"'), ("install_proton.sh", f'DEFAULT_VERSION="{version}"')):
        if stamp not in (folder / script).read_text(encoding="utf-8"):
            fail(f"{script} is not pinned to {version} (a tag run stamps it)")
    return [folder / n for n in PAYLOAD]


def linux_files(folder, version):
    names = [f"tpf2mp-linux-{version}-native.{ext}" for ext in ("run", "tar.gz", "sha256")]
    paths = [Path(folder) / n for n in names]
    missing = [p.name for p in paths if not p.is_file()]
    if missing:
        fail(f"--linux-dir lacks {', '.join(missing)} (tools/linux/build_release.sh --version {version}, then the -native copies)")
    listed = sums(paths[2].read_text())
    for p in paths[:2]:
        if listed.get(p.name) != sha256(p):
            fail(f"{p.name} does not match {paths[2].name}")
    return paths


def upload(gh, release, files, names=None):
    """Upload files (as `names`) to a release, skipping identical ones and replacing others."""
    have = {a["name"]: a for a in release.get("assets", [])}
    upload_url = release["upload_url"].split("{")[0]
    for i, path in enumerate(files):
        name = names[i] if names else path.name
        digest = "sha256:" + sha256(path)
        old = have.get(name)
        if old and old.get("digest") == digest:
            say(f"  {name}: already there")
            continue
        if old:
            gh.write(f"replace {name}", "DELETE", f"/repos/{release['_repo']}/releases/assets/{old['id']}")
        say(f"  {name}: uploading {path.stat().st_size} bytes")
        gh.write(f"upload {name}", "POST", f"{upload_url}?name={name}", data=path.read_bytes(), ctype="application/octet-stream")


def launchers(gh, args, into):
    want = [("windows", args.windows_launcher, r"-Setup\.exe$", WINDOWS_NAME)]
    if not args.no_linux_launcher:
        want.append(("linux", args.linux_launcher, r"\.AppImage$", LINUX_NAME))
    rel = gh.call("GET", f"/repos/{LAUNCHER_REPO}/releases/latest")
    args.launcher_release = rel
    listed = {}
    sums_asset = next((a for a in rel["assets"] if a["name"] == "SHA256SUMS.txt"), None)
    if sums_asset:
        listed = sums(urllib.request.urlopen(sums_asset["browser_download_url"], timeout=60).read().decode())
    out = []
    for kind, local, pattern, name in want:
        if local:
            path = Path(local)
            if not path.is_file():
                fail(f"no such {kind} launcher: {local}")
        else:
            asset = next((a for a in rel["assets"] if re.search(pattern, a["name"])), None)
            if asset is None:
                fail(f"{LAUNCHER_REPO} {rel['tag_name']} has no {kind} launcher; pass --{kind}-launcher FILE"
                     + (" or --no-linux-launcher" if kind == "linux" else ""))
            path = into / asset["name"]
            path.write_bytes(urllib.request.urlopen(asset["browser_download_url"], timeout=600).read())
            expected = listed.get(asset["name"]) or (asset.get("digest") or "")[7:]
            if expected and sha256(path) != expected:
                fail(f"{asset['name']} does not match {LAUNCHER_REPO} {rel['tag_name']}'s checksum")
            say(f"{kind} launcher: {asset['name']} from {LAUNCHER_REPO} {rel['tag_name']}")
        out.append((path, name))
    return out


def launcher_release(gh, args):
    """A launcher update on its own: launcher-v<version>, the two files, not marked Latest."""
    with tempfile.TemporaryDirectory(prefix="tpf2mp-launcher-") as tmp:
        chosen = launchers(gh, args, Path(tmp))
        source = args.launcher_release
        lver = source["tag_name"].lstrip("v")
        if not re.fullmatch(r"\d+\.\d+(\.\d+){0,2}", lver):
            fail(f"unexpected launcher tag {source['tag_name']}")
        tag = f"launcher-v{lver}"
        base = f"https://github.com/{REPO}/releases/download/{tag}"
        body = (f"## Launcher {lver}\n\n"
                "A new version of the launcher that installs, updates and starts TpF2 Multiplayer. "
                "It does not change the mod version: the launcher installs whichever mod release your group plays.\n\n"
                "| You play on | Get this one file |\n| --- | --- |\n"
                f"| **Windows** | **[{WINDOWS_NAME}]({base}/{WINDOWS_NAME})** |\n"
                + ("" if args.no_linux_launcher else f"| **Linux / Steam Deck** | **[{LINUX_NAME}]({base}/{LINUX_NAME})** |\n")
                + f"\nAn installed launcher also updates itself (Settings). What changed: [launcher {lver}]({source['html_url']}).\n")
        existing = find_release(gh, tag)
        if existing and not existing["draft"] and not args.dry_run:
            fail(f"{tag} is already published")
        rel = existing or gh.write(f"create {tag}", "POST", f"/repos/{REPO}/releases", body={
            "tag_name": tag, "target_commitish": "main", "name": f"Launcher {lver}", "body": body,
            "draft": True, "prerelease": False, "make_latest": "false"})
        if rel is None:
            say(f"  (dry run) {tag} would carry: {', '.join(n for _, n in chosen)}")
            return
        rel["_repo"] = REPO
        keep = {n for _, n in chosen}
        for a in rel.get("assets", []):
            if a["name"] not in keep:
                gh.write(f"remove {a['name']}", "DELETE", f"/repos/{REPO}/releases/assets/{a['id']}")
        rel["assets"] = [a for a in rel.get("assets", []) if a["name"] in keep]
        say(f"{tag}:")
        upload(gh, rel, [p for p, _ in chosen], [n for _, n in chosen])
        if args.publish:
            published(gh, rel, False)
            say(f"published: https://github.com/{REPO}/releases/tag/{tag}")
        else:
            say("left as a draft (--publish publishes it)")


# the direct installers the version's page carries beside the launchers
def page_direct(version):
    return ["TpF2Multiplayer.msi", "install_proton.sh", f"tpf2mp-linux-{version}-native.run", SERVER_NAME]


def launcher_table(tag, files_url, linux_launcher=True):
    """The page's Download section: the launchers, then the direct installers."""
    base = f"https://github.com/{REPO}/releases/download/{tag}"
    version = tag.lstrip("v")
    run = f"tpf2mp-linux-{version}-native.run"
    linux = (f"| **Linux / Steam Deck** (the native game or Proton) | **[{LINUX_NAME}]({base}/{LINUX_NAME})** -- make it executable, run it, then **Update & play** |\n"
             if linux_launcher else "")
    return ("## Download\n\n| You play on | Get this one file |\n| --- | --- |\n"
            f"| **Windows** (Steam, game build 35924) | **[{WINDOWS_NAME}]({base}/{WINDOWS_NAME})** -- install it, then **Update & play** |\n"
            + linux +
            "\nThe launcher installs this version and keeps it up to date. Or install this version directly:\n\n"
            "| You play on | Get this one file |\n| --- | --- |\n"
            f"| **Windows** | [TpF2Multiplayer.msi]({base}/TpF2Multiplayer.msi) -- close the game, run it |\n"
            f"| **Linux / Steam Deck**, the Windows game under Proton | [install_proton.sh]({base}/install_proton.sh) -- run it with sh; it fetches the rest itself |\n"
            f"| **Linux**, the native game | [{run}]({base}/{run}) -- close the game, `bash {run}` |\n"
            f"| **A dedicated server** on Linux or a VPS | [{SERVER_NAME}]({base}/{SERVER_NAME}) -- unpack it, `sudo sh tpf2mp-server/setup_vps.sh`; "
            f"the guide: [HOSTING_A_SERVER.md](https://github.com/{REPO}/blob/main/docs/HOSTING_A_SERVER.md). A Windows server is the MSI above |\n\n"
            f"The files zip, the Linux .tar.gz, install_proton.py and the checksums are in [the update files]({files_url}). "
            "The two *Source code* archives at the bottom are the repository, not the mod. Everyone in a session needs the same version.\n")


def download_section(body, tag, files_url, linux_launcher=True):
    """body with its "## Download" section (up to the next "## ") replaced by launcher_table."""
    rest = body.split("\n## ", 1)[1] if body.startswith("## Download") and "\n## " in body else None
    table = launcher_table(tag, files_url, linux_launcher)
    return table + "\n## " + rest if rest is not None else table + "\n" + body


def copy_release(gh, tag, version, prerelease, commit, notes, files, dry):
    """The release <version> (no "v") with exactly the install files, created as a draft."""
    copy_tag = version
    rel = find_release(gh, copy_tag)
    v_url = f"https://github.com/{REPO}/releases/tag/{tag}"
    pkg_url = f"https://github.com/{PACKAGES_REPO}/releases/tag/{tag}"
    body = (f"The update files of **[TpF2 Multiplayer {version}]({v_url})**. Players: get the launcher from "
            f"[that page]({v_url}); the launchers download these from [the packages release]({pkg_url}).\n\n" + notes)
    if rel is None:
        say(f"creating {copy_tag} (the update files)")
        rel = gh.write(f"create {copy_tag}", "POST", f"/repos/{REPO}/releases", body={
            "tag_name": copy_tag, "target_commitish": commit, "name": f"{version} (update files)", "body": body,
            "draft": True, "prerelease": prerelease, "make_latest": "false"})
    elif rel.get("body") != body:
        gh.write(f"update {copy_tag}'s notes", "PATCH", f"/repos/{REPO}/releases/{rel['id']}", body={"body": body})
    if rel is None:
        return None
    rel["_repo"] = REPO
    upload(gh, rel, files)
    return rel


def packages_release(gh, tag, version, prerelease, files):
    pkg = gh.get(f"/repos/{PACKAGES_REPO}/releases/tags/{tag}")
    if pkg is None:
        say(f"creating {PACKAGES_REPO} {tag}")
        pkg = gh.write("create the packages release", "POST", f"/repos/{PACKAGES_REPO}/releases", body={
            "tag_name": tag, "target_commitish": "main", "name": f"TpF2 Multiplayer {version} (install files)",
            "prerelease": prerelease, "make_latest": "false" if prerelease else "true",
            "body": f"Install files of [TpF2 Multiplayer {version}](https://github.com/{REPO}/releases/tag/{tag}), "
                    "downloaded by its launchers and by install_proton.sh. Players: get the launcher from that release."})
    if pkg is not None:
        pkg["_repo"] = PACKAGES_REPO
        say(f"packages release: {pkg['html_url']}")
        upload(gh, pkg, files)


def only(gh, rel, names, what):
    """Remove every asset of `rel` not named in `names` (after the new ones are up)."""
    for a in rel.get("assets", []):
        if a["name"] not in names:
            gh.write(f"remove {a['name']} from {what}", "DELETE", f"/repos/{REPO}/releases/assets/{a['id']}")
    rel["assets"] = [a for a in rel.get("assets", []) if a["name"] in names]


def notes_of(body):
    """A version's notes without the download table and without a note an earlier run put first."""
    for lead in ("The install files of ", "The update files of "):
        if body.startswith(lead) and "\n\n" in body:
            body = body.split("\n\n", 1)[1]
    if body.startswith("## Download") and "\n## " in body:
        body = "## " + body.split("\n## ", 1)[1]
    return body


def page_release(gh, args):
    """A version published with its files on v<version>: the files copied (packages
    release, <version>), then the launchers onto v<version> and its files removed."""
    tag = args.page_tag
    if not re.fullmatch(r"v\d+\.\d+(\.\d+){0,2}", tag or ""):
        fail("page needs the version's tag, e.g. page v0.7.0.5")
    version = tag[1:]
    mod = find_release(gh, tag)
    if mod is None or mod["draft"]:
        fail(f"{tag} is not a published release (a new version goes through publish_release.py {tag})")
    mod["_repo"] = REPO
    notes = notes_of(mod.get("body") or "")
    with tempfile.TemporaryDirectory(prefix="tpf2mp-page-") as tmp:
        tmp = Path(tmp)
        files = []
        for a in mod["assets"]:
            if a["name"] in (WINDOWS_NAME, LINUX_NAME):
                continue
            path = tmp / a["name"]
            path.write_bytes(urllib.request.urlopen(a["browser_download_url"], timeout=600).read())
            want = (a.get("digest") or "")[7:]
            if want and sha256(path) != want:
                fail(f"{a['name']} does not match its digest; nothing changed")
            files.append(path)
        if not any(f.name == "TpF2Multiplayer.msi" for f in files):
            copy = find_release(gh, version)
            if copy is None or not any(a["name"] == "TpF2Multiplayer.msi" for a in copy["assets"]):
                fail(f"{tag} has no install files and {version} none either")
            say(f"{tag} already has no install files; {version} holds them")
        else:
            say(f"{tag}: {len(files)} install file(s) downloaded and checked")
            packages_release(gh, tag, version, mod["prerelease"], files)
            copy = copy_release(gh, tag, version, mod["prerelease"], tag_commit(gh, tag), notes, files, args.dry_run)
            if copy is not None and args.publish and copy["draft"]:
                published(gh, copy, False)
        chosen = launchers(gh, args, tmp)
        direct = [f for f in files if f.name in page_direct(version)]
        if len(direct) < len(page_direct(version)):
            # the files were already moved off v<version>: take them from its packages release
            pkg = gh.get(f"/repos/{PACKAGES_REPO}/releases/tags/{tag}") or {"assets": []}
            for a in pkg["assets"]:
                if a["name"] in page_direct(version) and a["name"] not in {f.name for f in direct}:
                    path = tmp / a["name"]
                    path.write_bytes(urllib.request.urlopen(a["browser_download_url"], timeout=600).read())
                    want = (a.get("digest") or "")[7:]
                    if want and sha256(path) != want:
                        fail(f"{a['name']} does not match its digest; nothing changed")
                    direct.append(path)
        say(f"{tag} launchers and direct installers:")
        upload(gh, mod, [p for p, _ in chosen] + direct, [n for _, n in chosen] + [p.name for p in direct])
        only(gh, mod, {n for _, n in chosen} | {p.name for p in direct}, tag)
    pkg_url = f"https://github.com/{PACKAGES_REPO}/releases/tag/{tag}"
    gh.write(f"{tag}: the download table and Latest", "PATCH", f"/repos/{REPO}/releases/{mod['id']}", body={
        "name": version, "body": launcher_table(tag, pkg_url, not args.no_linux_launcher) + "\n" + notes,
        "make_latest": "false" if mod["prerelease"] else "true"})
    say(("(dry run) " if args.dry_run else "") + f"done: https://github.com/{REPO}/releases/tag/{tag}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tag", help="the release tag, e.g. v0.7.0.6; 'launcher' for a launcher update on its own; "
                                "'page' to move a published version to this layout")
    ap.add_argument("page_tag", nargs="?", help="with page: the version's tag, e.g. v0.7.0.5")
    ap.add_argument("--linux-dir", help="folder with tpf2mp-linux-<v>-native.run/.tar.gz/.sha256 (a mod version)")
    ap.add_argument("--payload-dir", help="folder with the Windows/Proton files instead of the tag run's artifact")
    ap.add_argument("--windows-launcher", help="a launcher Setup.exe instead of tearded's newest release")
    ap.add_argument("--linux-launcher", help="a Linux launcher AppImage instead of tearded's newest release")
    ap.add_argument("--no-linux-launcher", action="store_true", help="publish without a Linux launcher (none released yet)")
    ap.add_argument("--publish", action="store_true", help="publish the draft at the end (default: leave it a draft)")
    ap.add_argument("--dry-run", action="store_true", help="check everything, change nothing on GitHub")
    args = ap.parse_args()
    if args.tag == "launcher":
        return launcher_release(GitHub(token(), args.dry_run), args)
    if args.tag == "page":
        return page_release(GitHub(token(), args.dry_run), args)
    if not re.fullmatch(r"v\d+\.\d+(\.\d+){0,2}", args.tag):
        fail("the tag looks like v0.7.0.6, or is 'launcher'")
    if not args.linux_dir:
        fail("a mod version needs --linux-dir")
    version = args.tag[1:]
    gh = GitHub(token(), args.dry_run)

    mod = draft_release(gh, args.tag)
    say(f"release {args.tag}: {'draft' if mod['draft'] else 'PUBLISHED'}, {len(mod['assets'])} asset(s)")
    if not mod["draft"] and not args.dry_run:
        fail(f"{args.tag} is already published; this only completes a draft (--dry-run to look anyway)")
    mod["_repo"] = REPO
    body = mod.get("body") or ""
    if args.no_linux_launcher:
        # no Linux launcher yet: its row would link a file this release does not have
        pkg_url = f"https://github.com/{PACKAGES_REPO}/releases/download/{args.tag}"
        body = "\n".join(l if not l.startswith("| **Linux / Steam Deck**") else
                         f"| **Linux / Steam Deck** (the Windows game under Proton) | [install_proton.sh]({pkg_url}/install_proton.sh)"
                         f" -- run it with sh; it fetches the rest itself (the Linux launcher is on its way) |"
                         for l in body.split("\n"))
    with tempfile.TemporaryDirectory(prefix="tpf2mp-publish-") as tmp:
        tmp = Path(tmp)
        folder = Path(args.payload_dir) if args.payload_dir else payload_from_ci(gh, args.tag, tmp)
        files = check_payload(folder, version) + linux_files(args.linux_dir, version)
        say(f"install files: {len(files)} checked")
        chosen = launchers(gh, args, tmp)
        packages_release(gh, args.tag, version, mod["prerelease"], files)
        copy = copy_release(gh, args.tag, version, mod["prerelease"], mod.get("target_commitish") or "main",
                            notes_of(body), files, args.dry_run)
        direct = [f for f in files if f.name in page_direct(version)]
        say(f"{args.tag} launchers and direct installers:")
        upload(gh, mod, [p for p, _ in chosen] + direct, [n for _, n in chosen] + [p.name for p in direct])
        only(gh, mod, {n for _, n in chosen} | {p.name for p in direct}, args.tag)
    body = download_section(body, args.tag, f"https://github.com/{REPO}/releases/tag/{version}", not args.no_linux_launcher)
    if body != mod.get("body"):
        gh.write(f"update {args.tag}'s notes", "PATCH", f"/repos/{REPO}/releases/{mod['id']}", body={"body": body})
    if args.publish:
        if copy is not None:
            published(gh, copy, False)
        published(gh, mod, not mod["prerelease"])
        say(("(dry run) would be " if args.dry_run else "") + f"published: https://github.com/{REPO}/releases/tag/{args.tag} "
            f"(launchers), https://github.com/{REPO}/releases/tag/{version} (update files)")
    else:
        say("left as drafts (--publish publishes the update files, then the page)")

if __name__ == "__main__":
    main()
