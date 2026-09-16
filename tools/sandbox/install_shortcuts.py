#!/usr/bin/env python3
"""Install launch shortcuts for an already configured Transport Fever 2 lab."""
import argparse
from pathlib import Path
import shutil
import subprocess


def desktop_quote(value):
    # Desktop Entry Exec quoting is not shell quoting; % introduces field codes.
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$").replace("%", "%%") + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.home() / ".local/share/tpf2mp-lab")
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    if not (root / "lab.json").is_file():
        parser.error(f"No lab.json at {root}; configure the lab first")
    source = Path(__file__).resolve().parent
    applications = Path.home() / ".local/share/applications"
    applications.mkdir(parents=True, exist_ok=True)
    desktop = Path.home() / "Desktop"
    for actor, title in (("native", "Native Linux"), ("proton", "Proton")):
        entry = ("[Desktop Entry]\nType=Application\n"
                 f"Name=TpF2 Test — {title}\n"
                 f"Comment=Launch the isolated {title} multiplayer test instance\n"
                 f"Exec=/usr/bin/python3 {desktop_quote(source / 'launch.py')} {actor} --root {desktop_quote(root)}\n"
                 "Icon=steam_icon_1066780\nTerminal=false\nCategories=Game;\nStartupNotify=false\n")
        installed = applications / f"tpf2mp-lab-{actor}.desktop"
        installed.write_text(entry)
        installed.chmod(0o755)
        if shutil.which("desktop-file-validate"):
            subprocess.run(["desktop-file-validate", str(installed)], check=True)
        if desktop.is_dir():
            shortcut = desktop / installed.name
            shutil.copy2(installed, shortcut)
            if shutil.which("gio"):
                subprocess.run(["gio", "set", str(shortcut), "metadata::trusted", "true"], check=False)
        print(f"Installed: {title} ({installed})")
    local_bin = Path.home() / ".local/bin"
    local_bin.mkdir(parents=True, exist_ok=True)
    link = local_bin / "tpf2mp-lab"
    target = source / "tpf2mp-lab"
    if link.is_symlink() and link.resolve() == target:
        pass
    elif not link.exists() and not link.is_symlink():
        link.symlink_to(target)
    else:
        print(f"Kept existing {link}; run {target} directly")
    if shutil.which("update-desktop-database"):
        subprocess.run(["update-desktop-database", str(applications)], check=True)


if __name__ == "__main__":
    main()
