#!/usr/bin/env python3
"""Announce a stable release on a Discord webhook.

    python tools/release_webhook.py EVENT_JSON            posts to $RELEASE_WEBHOOK_URL
    python tools/release_webhook.py EVENT_JSON --dry-run  prints the payload instead

Run by .github/workflows/release-webhook.yml on `release: released` with the event
file GitHub hands the job ($GITHUB_EVENT_PATH). One version publishes several
releases (tools/publish_release.py): the page v<version> (the one players see,
marked Latest), the update files <version> without the "v", launcher-v<version>
for launcher updates, and experimental versions as pre-releases. Only the first
is announced: a published, non-pre-release release whose tag is v<digits>.<...>.

The message is the version's title, its link, and the "### Changes" section of
the release notes (installer/RELEASE-<version>.md, which build-msi.yml puts under
the download tables), cut to what an embed holds.
"""
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request

STABLE_TAG = re.compile(r"^v\d+(\.\d+)+$")
EMBED_DESCRIPTION_MAX = 4096   # Discord's limit for an embed description
EMBED_TITLE_MAX = 256
COLOR = 0x2E7D32


def is_stable(release):
    """Why a release is not announced, or None when it is."""
    if release.get("draft"):
        return "a draft"
    if release.get("prerelease"):
        return "a pre-release"
    if not STABLE_TAG.match(release.get("tag_name") or ""):
        return f"tag {release.get('tag_name')!r} is not a version page (v<version>)"
    return None


def changes(body):
    """The "### Changes" section of the notes, else the text after the download tables."""
    body = (body or "").replace("\r\n", "\n")
    m = re.search(r"^###\s+Changes\s*\n(.*?)(?=^#{1,3}\s|\Z)", body, re.M | re.S)
    if m:
        return m.group(1).strip()
    # no Changes heading: drop the "## Download" block (tables do not render in Discord)
    body = re.sub(r"^## Download\n.*?(?=^## |\Z)", "", body, flags=re.M | re.S)
    return body.strip()


def headline(body):
    """The notes' own "## <version> - <summary>" heading, if there is one."""
    for m in re.finditer(r"^##\s+(.+?)\s*$", (body or "").replace("\r\n", "\n"), re.M):
        if m.group(1) != "Download":
            return m.group(1)
    return None


def clip(text, limit, tail):
    if len(text) <= limit:
        return text
    cut = text[: limit - len(tail)]
    # end on a whole line where one is close enough
    nl = cut.rfind("\n")
    if nl > limit // 2:
        cut = cut[:nl]
    return cut.rstrip() + tail


def payload(release):
    url = release["html_url"]
    title = headline(release.get("body")) or release.get("name") or release["tag_name"]
    text = changes(release.get("body"))
    more = f"\n\n[Full release notes and downloads]({url})"
    description = clip(text, EMBED_DESCRIPTION_MAX - len(more), "\n...") + more
    return {
        "username": "TpF2 Multiplayer",
        "content": f"**New stable release: {release['tag_name']}** -- update with **Update & play** in the launcher.",
        "embeds": [{
            "title": clip(title, EMBED_TITLE_MAX, "..."),
            "url": url,
            "description": description.strip(),
            "color": COLOR,
            "timestamp": release.get("published_at"),
        }],
        "allowed_mentions": {"parse": []},
    }


def post(url, data):
    req = urllib.request.Request(
        url, data=json.dumps(data).encode("utf-8"), method="POST",
        headers={"Content-Type": "application/json", "User-Agent": "tpf2-multiplayer-release-webhook"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status
    except urllib.error.HTTPError as e:
        sys.exit(f"webhook refused the post: HTTP {e.code} {e.read()[:500]!r}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("event", help="the release event JSON ($GITHUB_EVENT_PATH)")
    ap.add_argument("--dry-run", action="store_true", help="print the payload, post nothing")
    args = ap.parse_args()

    with open(args.event, encoding="utf-8") as f:
        release = json.load(f)["release"]
    why = is_stable(release)
    if why:
        print(f"{release.get('tag_name')}: not announced, {why}")
        return
    data = payload(release)
    if args.dry_run:
        print(json.dumps(data, indent=2, ensure_ascii=False))
        return
    url = os.environ.get("RELEASE_WEBHOOK_URL", "").strip()
    if not url:
        print("::warning::RELEASE_WEBHOOK_URL is not set (repository secret): nothing posted")
        return
    status = post(url, data)
    print(f"{release['tag_name']}: announced (HTTP {status})")


if __name__ == "__main__":
    main()
