"""Compile/run the real Steam Messages adapter against a fake API boundary."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="tpf2-steam-messages-") as folder:
    out = Path(folder)
    script = out / "run.cmd"
    script.write_text("\n".join([
        "@echo off",
        f'call "{ROOT / "tools/msvc_env.bat"}" || exit /b 1',
        f'cl /nologo /utf-8 /std:c++17 /EHsc /MT "{ROOT / "tools/steam_messages_test.cpp"}" '
        f'/Fe:"{out / "test.exe"}" /Fo:"{out / "test.obj"}" || exit /b 1',
        f'"{out / "test.exe"}" || exit /b 1',
    ]) + "\n", encoding="utf-8")
    subprocess.run(["cmd", "/d", "/c", str(script)], cwd=ROOT, check=True)
