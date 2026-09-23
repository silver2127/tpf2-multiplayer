"""Build and exercise the real menu renderer without installing or starting the game."""
import os
import shutil
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="tpf2-title-test-") as folder:
        out = Path(folder)
        commands = [
            "@echo off",
            'set "TPF2_BUILD_NO_DEPLOY=1"',
            f'call "{ROOT / "tools/msvc_env.bat"}" || exit /b 1',
            f'call "{ROOT / "native/build.bat"}" menu || exit /b 1',
            f'cd /d "{ROOT}"',
        ]
        support = " ".join(f'"{ROOT / "native/out" / name}"' for name in (
            "hook_menu.obj", "native_io.obj", "native_control.obj", "gameuirelay_menu.obj"))
        for name in ("menu_title_panel_test", "lobby_save_picker_test"):
            commands += [
                f'cl /nologo /utf-8 /std:c++17 /EHsc /MT "{ROOT / "tools" / (name + ".cpp")}" '
                f'{support} user32.lib gdi32.lib advapi32.lib /Fe:"{out / (name + ".exe")}" '
                f'/Fo:"{out / (name + ".obj")}" || exit /b 1',
                f'"{out / (name + ".exe")}" "{out / (name + "-fixture")}" || exit /b 1',
            ]
        script = out / "run.cmd"
        script.write_text("\n".join(commands) + "\n", encoding="utf-8")
        env = dict(os.environ, TPF2_BUILD_NO_DEPLOY="1")
        subprocess.run(["cmd", "/d", "/c", str(script)], cwd=ROOT, env=env, check=True)
        if os.environ.get("TPF2_TEST_ARTIFACTS"):
            shutil.copytree(out / "menu_title_panel_test-fixture", os.environ["TPF2_TEST_ARTIFACTS"], dirs_exist_ok=True)


if __name__ == "__main__":
    main()
