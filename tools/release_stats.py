"""Download counts of every GitHub release asset -- the only place GitHub keeps
them is the API; the release pages show none.

    python tools\\release_stats.py                 # tpf2-multiplayer and its packages (0.7.0.6 on)
    python tools\\release_stats.py silver2127/tpf2-bigmap
    python tools\\release_stats.py --all           # every asset, not only the MSIs

Unauthenticated: 60 requests an hour per address, one request per 100 releases.
"""
import json
import sys
import urllib.request


def main(argv):
    repos = [a for a in argv[1:] if "/" in a]
    if not repos:
        # from 0.7.0.6 the MSI is downloaded from the packages repository, the launchers from the mod's releases
        repos = ["silver2127/tpf2-multiplayer", "silver2127/tpf2-multiplayer-packages"]
    for repo in repos:
        report(repo, argv)


def report(repo, argv):
    show_all = "--all" in argv
    releases = []
    page = 1
    while True:
        req = urllib.request.Request(f"https://api.github.com/repos/{repo}/releases?per_page=100&page={page}",
                                     headers={"Accept": "application/vnd.github+json", "User-Agent": "tpf2mp-release-stats"})
        with urllib.request.urlopen(req, timeout=30) as r:
            batch = json.loads(r.read())
        if not batch:
            break
        releases += batch
        page += 1
    total = msi_total = 0
    print(f"{repo}: {len(releases)} releases")
    print(f"{'release':18s} {'date':10s} {'all':>6s} {'msi':>6s}")
    for rel in releases:
        assets = rel.get("assets", [])
        n = sum(a.get("download_count", 0) for a in assets)
        msi = sum(a.get("download_count", 0) for a in assets if a["name"].lower().endswith(".msi"))
        total += n
        msi_total += msi
        print(f"{(rel.get('tag_name') or rel.get('name') or '?'):18s} {rel.get('published_at', '')[:10]:10s} {n:6d} {msi:6d}")
        if show_all:
            for a in assets:
                print(f"    {a['name']:40s} {a.get('download_count', 0):6d}")
    print(f"{'total':18s} {'':10s} {total:6d} {msi_total:6d}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
