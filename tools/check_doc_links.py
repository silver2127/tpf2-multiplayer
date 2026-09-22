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
SKIP_DIRS = ('.git', 'out', 'node_modules', '.local-test', 'dist', 'build', 'linux_port')   # linux_port: the porter's prompts describe the linux-native branch


def slug(h):
    h = re.sub(r'[`*_]', '', h.strip().lower())
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


def main(argv):
    root = os.path.abspath(os.getcwd())
    docs = ['README.md'] if '--readme' in argv else all_md(root)
    text, heads = {}, {}
    for p in all_md(root):
        with open(p, encoding='utf-8', errors='replace') as f:
            text[p] = f.read()
        heads[p] = {slug(m.group(1)) for m in HEAD.finditer(text[p])}

    dead_links, dead_anchors, dead_paths, linked = [], [], [], set()
    queue, done = list(docs), set()
    while queue:
        p = queue.pop(0)
        if p in done or p not in text:
            continue
        done.add(p)
        for label, target in LINK.finditer(text[p]) and [(m.group(1), m.group(2)) for m in LINK.finditer(text[p])]:
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
            if not os.path.exists(path):
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
