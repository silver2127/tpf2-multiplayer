r"""Check the documentation for dead references.

    python tools/check_doc_links.py            # every .md in the repo
    python tools/check_doc_links.py --readme   # only what README.md reaches

Three checks, all against the working tree:
  1. relative Markdown links whose target file does not exist
  2. anchor links (#heading) whose heading is not in the target document
  3. repository paths mentioned in prose or code spans (tools\x.py, docs/y.md,
     native/src/z.cpp, ...) that do not exist -- a deleted script or document
     is usually still named somewhere

Exits 1 when anything is dead. Reports orphan docs (no document links to them)
as information only.
"""
import os
import re
import sys

LINK = re.compile(r'\[([^\]]*)\]\(([^)\s]+)(?:\s+"[^"]*")?\)')
HEAD = re.compile(r'^#+\s+(.*?)\s*#*\s*$', re.M)
PATH = re.compile(r'(?<![\w/.:-])((?:tools|docs|native/src|netpunch|installer|mod/mp_lockstep_1|\.github/workflows)[/\\][\w./\\-]*\.(?:py|ps1|sh|cpp|h|asm|bat|lua|md|yml|wxs|txt|inl))\b')
ANCHOR = re.compile(r'<a\s+(?:id|name)="([^"]+)"')
CODE = re.compile(r'```.*?```|`(?:[^`\n]|\n(?![ \t]*\n))+`', re.S)   # a code span may wrap, not cross a paragraph
SKIP_DIRS = ('.git', 'out', 'node_modules', '.local-test', 'dist', 'build', 'linux_port')   # linux_port: the porter's prompts describe the linux-native branch
# Big Maps is mirrored to its own repository (tools/bigmap_sync), so its documents
# (and the README fragments the sync splices in) name paths from bigmap/, not from here.
SUBPROJECTS = {'bigmap/': 'bigmap', 'tools/bigmap_sync/readme/': 'bigmap'}
# Paths that exist only in the standalone Big Maps repository (tools/bigmap_sync/sync.py OWNED).
STANDALONE_OWNED = ('installer/', 'tools/vendor_host.ps1', 'tools/test_config_msi.py', 'tools/test_coexist.ps1')
# Named on purpose although they are not in this tree: records of what a past commit
# changed, files the software writes at run time, and files deliberately never added.
IGNORE = {
    ('bigmap/docs/terrain-lodtess.md', 'tools/test_terrain_lodtess.py'),       # "deliberately not added"
    ('bigmap/docs/linux/PORT.md', 'tools/linux/build.sh'),                     # the standalone repo's Linux build
    ('bigmap/docs/linux/PORT.md', 'tools/linux/package.sh'),
    ('docs/linux/NETPUNCH.md', 'netpunch/desynclogs.py'),                      # port record, file since removed
    ('docs/linux/UPSTREAM_dev_d129fab7.md', 'netpunch/player_stats.py'),       # integration record
    ('docs/linux/UPSTREAM_dev_d129fab7.md', 'tools/test_player_stats.py'),
    ('docs/linux/UPSTREAM_dev_b5dade06.md', 'tools/company_name_test.py'),    # historical integration; since removed
    ('installer/RELEASE-0.7.md', 'netpunch/tpf2mp_live_join.txt'),            # written at run time
}


def slug(h):
    h = re.sub(r'[`*]', '', h.strip().lower())   # GitHub keeps underscores
    h = re.sub(r'[^\w\- ]', '', h)
    return h.replace(' ', '-')


def norm(p):
    return os.path.normpath(p).replace('\\', '/')


def all_md(root):
    out = []
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        out += [norm(os.path.relpath(os.path.join(d, f), root)) for f in files if f.lower().endswith('.md')]
    return sorted(out)


def path_exists(doc, path):
    """A path named in `doc` exists from the repository root, or from the root
    of the subproject `doc` belongs to."""
    if os.path.exists(path) or (doc, path) in IGNORE:
        return True
    for prefix, base in SUBPROJECTS.items():
        if doc.startswith(prefix) and (os.path.exists(os.path.join(base, path))
                                       or path.startswith(STANDALONE_OWNED)):
            return True
    return False


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    docs = ['README.md'] if '--readme' in argv else all_md(root)
    text, heads = {}, {}
    for p in all_md(root):
        with open(p, encoding='utf-8', errors='replace') as f:
            text[p] = f.read()
        heads[p] = {slug(m.group(1)) for m in HEAD.finditer(text[p])} | \
                   {m.group(1).lower() for m in ANCHOR.finditer(text[p])}

    dead_links, dead_anchors, dead_paths, linked = [], [], [], set()
    queue, done = list(docs), set()
    while queue:
        p = queue.pop(0)
        if p in done or p not in text:
            continue
        done.add(p)
        prose = CODE.sub('', text[p])   # a [x](y) inside code is code, not a link
        for m in LINK.finditer(prose):
            label, target = m.group(1), m.group(2)
            if target.startswith(('http://', 'https://', 'mailto:')):
                continue
            file, _, anchor = target.partition('#')
            full = norm(os.path.join(os.path.dirname(p), file)) if file else p
            if file and not os.path.exists(full):
                dead_links.append((p, label, target))
                continue
            if file:
                linked.add(full)
                if full in text and full not in done and '--readme' in argv:
                    queue.append(full)
            if anchor and full in heads and anchor.lower() not in heads[full]:
                dead_anchors.append((p, label, target))
        for m in PATH.finditer(text[p]):
            path = norm(m.group(1))
            if not path_exists(p, path):
                dead_paths.append((p, m.group(1)))

    def show(title, rows, fmt):
        print(f"\n{title}: {len(rows)}")
        for r in rows:
            print("  " + fmt(*r))
    print(f"documents checked: {len(done)}")
    show("dead file links", dead_links, lambda p, l, t: f"{p}: [{l}]({t})")
    show("dead anchors", dead_anchors, lambda p, l, t: f"{p}: [{l}]({t})")
    show("paths named in prose that do not exist", sorted(set(dead_paths)), lambda p, t: f"{p}: {t}")
    if '--readme' not in argv:
        orphans = [p for p in text if p not in linked and p != 'README.md' and not p.startswith('docs/re/')
                   and not re.match(r'installer/RELEASE-', p)]   # release notes are read on the release page
        show("orphan documents (nothing links to them)", [(p,) for p in orphans], lambda p: p)
    return 1 if (dead_links or dead_anchors or dead_paths) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
