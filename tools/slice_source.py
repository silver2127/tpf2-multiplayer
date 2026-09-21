"""The slice's source as one text: slice_hook.cpp with its native/src/slice/*.inl includes inlined.

Tests that check bytes, prologues or decoders against the source read it through
this, so a text anchor works whichever part its lines live in.

    from slice_source import slice_source
    source = slice_source(repo)     # repo: the repository root (a pathlib.Path or str)
"""
import re
from pathlib import Path

_INC = re.compile(r'^#include "slice/([A-Za-z0-9_.]+)"')


def slice_source(repo):
    src = Path(repo) / "native" / "src"
    out = []
    for line in (src / "slice_hook.cpp").read_text(encoding="utf-8").splitlines(keepends=True):
        m = _INC.match(line)
        if m:
            out.append((src / "slice" / m.group(1)).read_text(encoding="utf-8"))
            if not out[-1].endswith("\n"):
                out.append("\n")
        else:
            out.append(line)
    return "".join(out)
