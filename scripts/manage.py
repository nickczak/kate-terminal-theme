#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install selected Kate settings and user-local assets, with reversible changes."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[1]
BLOCK_START = "# >>> kate-terminal-theme >>>"
BLOCK_END = "# <<< kate-terminal-theme <<<"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_text(path: Path) -> str:
    return path.read_text() if path.exists() else ""


def atomic_write(path: Path, data: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=".kate-theme-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
        os.chmod(name, mode)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def setting(text: str, group: str, key: str) -> str | None:
    section = None
    result = None
    for line in text.splitlines():
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif section == group and line.startswith(key + "="):
            result = line[len(key) + 1:]
    return result


def update_setting(text: str, group: str, key: str, value: str | None) -> str:
    """Edit one KConfig key without parsing/reformatting unrelated settings."""
    lines = text.splitlines(keepends=True)
    start = next((i for i, line in enumerate(lines) if line.rstrip("\r\n") == f"[{group}]"), None)
    if start is None:
        if value is None:
            return text
        return text.rstrip("\n") + ("\n\n" if text else "") + f"[{group}]\n{key}={value}\n"
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
    matches = [i for i in range(start + 1, end) if lines[i].startswith(key + "=")]
    if matches:
        for i in reversed(matches):
            lines[i:i + 1] = [] if value is None or i != matches[0] else [f"{key}={value}\n"]
    elif value is not None:
        if end and not lines[end - 1].endswith("\n"):
            lines[end - 1] += "\n"
        lines.insert(end, f"{key}={value}\n")
    return "".join(lines)


class Changes:
    """Track original content once, even across repeated installs and updates."""
    def __init__(self, state_dir: Path):
        self.directory = state_dir
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.directory.chmod(0o700)
        self.path = state_dir / "install.json"
        self.data = json.loads(self.path.read_text()) if self.path.exists() else {
            "version": 1, "files": {}, "configs": {}, "config_originals": {}, "blocks": {}
        }
        if self.data.get("version") != 1:
            raise RuntimeError("Unsupported installation manifest version")

    def save(self):
        atomic_write(self.path, (json.dumps(self.data, indent=2) + "\n").encode(), 0o600)

    def backup(self, path: Path) -> dict:
        if not path.exists():
            return {"exists": False}
        if not path.is_file():
            raise RuntimeError(f"Expected a file: {path}")
        name = "backups/" + digest(str(path).encode())
        target = self.directory / name
        if not target.exists():
            atomic_write(target, path.read_bytes(), 0o600)
        return {"exists": True, "backup": name, "mode": stat.S_IMODE(path.stat().st_mode)}

    def put(self, path: Path, content: bytes | str, mode=0o644):
        path = path.resolve()
        content = content.encode() if isinstance(content, str) else content
        entry = self.data["files"].setdefault(str(path), {"before": self.backup(path)})
        entry.update(installed_sha256=digest(content), removed=False)
        self.save()
        atomic_write(path, content, mode)

    def remove_legacy(self, path: Path):
        path = path.resolve()
        if not path.exists():
            return
        entry = self.data["files"].setdefault(str(path), {"before": self.backup(path)})
        entry.update(installed_sha256=None, removed=True)
        self.save()
        path.unlink()

    def patch(self, path: Path, group: str, key: str, value: str | None):
        path = path.resolve()
        text = read_text(path)
        self.data["config_originals"].setdefault(str(path), self.backup(path))
        record_key = json.dumps([str(path), group, key])
        record = self.data["configs"].setdefault(record_key, {
            "path": str(path), "group": group, "key": key, "before": setting(text, group, key)
        })
        record["installed"] = value
        self.save()
        mode = stat.S_IMODE(path.stat().st_mode) if path.exists() else 0o600
        atomic_write(path, update_setting(text, group, key, value).encode(), mode)

    def block(self, path: Path, body: str):
        path = path.resolve()
        text = read_text(path)
        pattern = re.compile(re.escape(BLOCK_START) + r"\n.*?" + re.escape(BLOCK_END) + r"\n?", re.S)
        match = pattern.search(text)
        block = f"{BLOCK_START}\n{body.rstrip()}\n{BLOCK_END}\n"
        record = self.data["blocks"].setdefault(str(path), {"before": match.group(0) if match else ""})
        record["installed"] = block
        record.setdefault("file_existed", path.exists())
        self.save()
        text = pattern.sub(lambda _: block, text, count=1) if match else text.rstrip("\n") + ("\n\n" if text else "") + block
        atomic_write(path, text.encode(), stat.S_IMODE(path.stat().st_mode) if path.exists() else 0o644)

    def uninstall(self, force=False) -> list[str]:
        skipped = []
        # Restore only keys whose values still match what the installer set.
        for record in self.data["configs"].values():
            path = Path(record["path"])
            text = read_text(path)
            if not force and setting(text, record["group"], record["key"]) != record["installed"]:
                skipped.append(f"modified setting {path}: [{record['group']}] {record['key']}")
                continue
            if path.exists():
                atomic_write(path, update_setting(text, record["group"], record["key"], record["before"]).encode(), stat.S_IMODE(path.stat().st_mode))
        for name, original in self.data["config_originals"].items():
            path = Path(name)
            if not original["exists"] and path.exists():
                contents = path.read_text()
                if not any(line.strip() and not line.startswith(("[", "#", ";")) for line in contents.splitlines()):
                    path.unlink()
        for name, record in self.data["blocks"].items():
            path = Path(name)
            text = read_text(path)
            if record["installed"] not in text:
                skipped.append(f"modified tmux include in {path}")
                continue
            text = text.replace(record["installed"], record["before"], 1)
            if not record["file_existed"] and not text.strip():
                path.unlink(missing_ok=True)
            else:
                atomic_write(path, text.encode(), stat.S_IMODE(path.stat().st_mode))
        for name, entry in reversed(list(self.data["files"].items())):
            path = Path(name)
            matches = not path.exists() if entry["removed"] else path.is_file() and digest(path.read_bytes()) == entry["installed_sha256"]
            if not force and not matches:
                skipped.append(f"modified file {path}")
                continue
            before = entry["before"]
            if before["exists"]:
                atomic_write(path, (self.directory / before["backup"]).read_bytes(), before["mode"])
            else:
                path.unlink(missing_ok=True)
        if not skipped:
            self.path.unlink(missing_ok=True)
        return skipped


class Layout:
    def __init__(self, args):
        prefix = args.prefix.expanduser().resolve() if args.prefix else None
        self.config = prefix / "config" if prefix else Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")).expanduser().resolve()
        self.data = prefix / "data" if prefix else Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")).expanduser().resolve()
        self.cache = prefix / "cache" if prefix else Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")).expanduser().resolve()
        state_home = prefix / "state" if prefix else Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")).expanduser().resolve()
        self.state = args.state_dir.expanduser().resolve() if args.state_dir else state_home / "kate-terminal-theme"
        self.bin = prefix / "bin" if prefix else Path.home() / ".local/bin"
        self.tmux = prefix / "tmux.conf" if prefix else Path.home() / ".tmux.conf"
        self.package = self.data / "kate-terminal-theme"
        self.plugins = self.package / "plugins"


def recolor_icon() -> tuple[str | None, Path | None]:
    roots = [Path(p) / "icons" for p in os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(":")]
    for root in roots:
        for relative in ["hicolor/scalable/apps/kate.svg", "breeze/apps/48/kate.svg", "breeze-dark/apps/48/kate.svg"]:
            path = root / relative
            if not path.exists():
                continue
            text = path.read_text()
            # Use the installed Kate artwork; redistribute no mascot artwork in this repo.
            text, circles = re.subn(r'(<circle\b[^>]*\bfill=")[^"]*(")', r'\g<1>#000000\2', text, count=1)
            text, ellipses = re.subn(r'(<ellipse\b[^>]*\bfill=")[^"]*(")', r'\g<1>#000000\2', text, count=1)
            if circles and ellipses:
                text = text.replace('fill="url(#b)"', 'fill="#000000"', 1)
                ET.fromstring(text)
                return text, root
    return None, None


def dependency_hint() -> str:
    if shutil.which("dnf"):
        return "sudo dnf install python3 kate konsole cmake gcc-c++ extra-cmake-modules qt6-qtbase-devel qt6-qtsvg-devel kf6-ktexteditor-devel kf6-kcoreaddons-devel kf6-kparts-devel google-noto-sans-mono-fonts"
    if shutil.which("apt-get"):
        return "sudo apt install python3 kate konsole cmake g++ extra-cmake-modules qt6-base-dev qt6-svg-dev libkf6texteditor-dev libkf6coreaddons-dev libkf6parts-dev fonts-noto-mono"
    if shutil.which("pacman"):
        return "sudo pacman -S --needed python kate konsole cmake gcc extra-cmake-modules qt6-base qt6-svg ktexteditor kcoreaddons kparts noto-fonts"
    return "Install Kate (Qt 6), Konsole, CMake, a C++17 compiler, Qt 6 Widgets/SVG, and KDE Frameworks 6 TextEditor/CoreAddons/Parts development packages."


def build_plugin(layout: Layout, args) -> Path:
    for executable in ["cmake", "kate", "konsole"]:
        if not shutil.which(executable):
            raise RuntimeError(f"Missing {executable}. Install prerequisites first:\n  {dependency_hint()}")
    build = layout.cache / "kate-terminal-theme/build"
    if args.check:
        print(f"Would build the plugin in {build}")
        return build / "kateterminalex.so"
    # A moved clone must not reuse a CMake cache pointing to its old source tree.
    cmake_cache = build / "CMakeCache.txt"
    if cmake_cache.exists():
        source = next((line.split("=", 1)[1] for line in cmake_cache.read_text().splitlines() if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")), "")
        if source != str(REPO / "plugin"):
            shutil.rmtree(build)
    configure = ["cmake", "-S", str(REPO / "plugin"), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DBUILD_KATE_INTEGRATION_TESTS=OFF"]
    try:
        subprocess.run(configure, check=True)
        subprocess.run(["cmake", "--build", str(build), "--parallel", str(min(os.cpu_count() or 2, 4))], check=True)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"Plugin build failed. No Kate settings were changed.\nPrerequisites:\n  {dependency_hint()}") from error
    return build / "kateterminalex.so"


def launcher(layout: Layout, kate: str) -> str:
    q = shlex.quote
    return f'''#!/bin/sh
# Generated by kate-terminal-theme. Does not depend on the cloned repository.
export XDG_CONFIG_HOME={q(str(layout.config))}
export XDG_DATA_HOME={q(str(layout.data))}
export XDG_CACHE_HOME={q(str(layout.cache))}
export XDG_STATE_HOME={q(str(layout.state.parent))}
kate_theme_plugins={q(str(layout.plugins))}
case ":${{QT_PLUGIN_PATH:-}}:" in
    *":$kate_theme_plugins:"*) ;;
    *) QT_PLUGIN_PATH="$kate_theme_plugins${{QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}}" ;;
esac
export QT_PLUGIN_PATH
exec {q(kate)} -stylesheet {q(str(layout.config / 'kate-terminal.qss'))} "$@"
'''


def desktop_entry(layout: Layout, icon: Path | None) -> str:
    # Desktop Exec quoting follows the desktop-entry format, rather than a shell.
    wrapper = str(layout.bin / "kate").replace("\\", "\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$").replace("%", "%%")
    return f'''[Desktop Entry]
Type=Application
Name=Kate
GenericName=Advanced Text Editor
Comment=Kate with GitHub Black and Vim-style Projects navigation
Exec="{wrapper}" -b %U
Icon={icon if icon else 'kate'}
Terminal=false
StartupNotify=true
StartupWMClass=kate
Categories=Qt;KDE;Development;TextEditor;
MimeType=text/plain;inode/directory;
Actions=NewWindow;

[Desktop Action NewWindow]
Name=New Window
Exec="{wrapper}" --startanon
'''


def refresh_caches(layout: Layout):
    command = shutil.which("update-desktop-database")
    if command:
        subprocess.run([command, str(layout.data / "applications")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    # A running desktop may use a different XDG root; refreshing its global
    # service cache is deliberately left to the desktop after a normal restart.


def install(args, layout: Layout):
    if not (REPO / "assets/settings.json").exists():
        raise RuntimeError("Run install.sh from a complete clone of the repository.")
    plugin = build_plugin(layout, args)
    icon_text, system_icons = recolor_icon() if not args.no_icon else (None, None)
    if args.check:
        print(f"Config: {layout.config}\nAssets: {layout.package}\nLauncher: {layout.bin / 'kate'}\nState/backups: {layout.state}")
        print("Check only: no files changed. Run without --check to build and install.")
        return
    changes = Changes(layout.state)
    changes.data["layout"] = {"config": str(layout.config), "data": str(layout.data), "bin": str(layout.bin)}
    try:
        for asset in sorted((REPO / "assets/tree").glob("*.svg")):
            changes.put(layout.package / "tree" / asset.name, asset.read_bytes())
        asset_dir = str(layout.package / "tree")
        if any(ch in asset_dir for ch in ["\n", "\r", '"', "\\"]):
            raise RuntimeError("The data path cannot contain newlines, double quotes, or backslashes.")
        stylesheet = (REPO / "assets/kate-terminal.qss.in").read_text().replace("@TREE_ASSET_DIR@", asset_dir)
        # QSS URL quotes are needed when the installation directory contains spaces.
        stylesheet = re.sub(r'url\(([^)]*)\)', lambda m: 'url("' + m[1] + '")', stylesheet)
        changes.put(layout.config / "kate-terminal.qss", stylesheet)
        changes.put(layout.data / "org.kde.syntax-highlighting/themes/GitHub Black.theme", (REPO / "themes/GitHub Black.theme").read_bytes())
        changes.put(layout.data / "color-schemes/TerminalBlack.colors", (REPO / "themes/TerminalBlack.colors").read_bytes())
        for asset in (REPO / "konsole").iterdir():
            changes.put(layout.data / "konsole" / asset.name, asset.read_bytes())
        changes.put(layout.plugins / "kf6/ktexteditor/kateterminalex.so", plugin.read_bytes(), 0o755)
        if not args.prefix:
            for name in ["kateterminalex.so", "libkateterminalex.so"]:
                changes.remove_legacy(Path.home() / ".local/lib/qt6/plugins/kf6/ktexteditor" / name)
        icon = None
        if icon_text:
            icon = layout.package / "icons/kate-terminal-black.svg"
            changes.put(icon, icon_text)
            for theme, relative in [("breeze", "apps/48/kate.svg"), ("breeze-dark", "apps/48/kate.svg"), ("hicolor", "scalable/apps/kate.svg")]:
                index = layout.data / "icons" / theme / "index.theme"
                upstream = system_icons / theme / "index.theme"
                if not index.exists() and upstream.exists():
                    changes.put(index, upstream.read_bytes())
                changes.put(layout.data / "icons" / theme / relative, icon_text)
        elif not args.no_icon:
            print("Note: no compatible Kate SVG was found; keeping the system app icon.")
        settings = json.loads((REPO / "assets/settings.json").read_text())
        for group, values in settings.items():
            for key, value in values.items():
                changes.patch(layout.config / "katerc", group, key, value)
        sessions = {layout.data / "kate/anonymous.katesession", *(layout.data / "kate/sessions").glob("*.katesession")}
        for session in sorted(sessions):
            for key, value in settings["Kate Plugins"].items():
                changes.patch(session, "Kate Plugins", key, value)
            # Reproduce the uncluttered sidebars without copying window state,
            # document paths, recent files, or any other session content.
            for tool in ["diagnostics", "output", "kate_plugin_katesearch", "kate_private_plugin_katefiletreeplugin", "kate_private_plugin_katekonsoleplugin", "kateproject", "kateprojectgit", "kateprojectinfo", "lspclient_symbol_outline"]:
                changes.patch(session, "MainWindow0", f"Kate-MDI-ToolView-{tool}-Show-Button-In-Sidebar", "false")
        kate_binary = "/usr/bin/kate" if Path("/usr/bin/kate").exists() else shutil.which("kate")
        changes.put(layout.bin / "kate", launcher(layout, kate_binary), 0o755)
        changes.put(layout.data / "applications/org.kde.kate.desktop", desktop_entry(layout, icon))
        changes.put(layout.package / "installer/manage.py", Path(__file__).read_bytes())
        uninstall_command = f"#!/bin/sh\nexec python3 {shlex.quote(str(layout.package / 'installer/manage.py'))} uninstall --state-dir {shlex.quote(str(layout.state))} \"$@\"\n"
        changes.put(layout.bin / "kate-terminal-uninstall", uninstall_command, 0o755)
        diag = f'''#!/bin/sh
umask 077
mkdir -p {shlex.quote(str(layout.state))} || exit 1
export KATE_TERMINAL_DIAGNOSTICS={shlex.quote(str(layout.state / 'runtime.jsonl'))}
: > "$KATE_TERMINAL_DIAGNOSTICS"
printf '%s\\n' 'Type :Ex in the diagnostic Kate window; close the window when finished.'
{shlex.quote(str(layout.bin / 'kate'))} --block --startanon "$@" 2> {shlex.quote(str(layout.state / 'plugin-loader.log'))}
'''
        changes.put(layout.bin / "kate-terminal-diagnose", diag, 0o755)
        if args.with_tmux:
            theme = layout.package / "tmux/github-dark.conf"
            changes.put(theme, (REPO / "tmux/github-dark.conf").read_bytes())
            escaped = str(theme).replace("\\", "\\\\").replace('"', '\\"')
            changes.block(layout.tmux, f'source-file "{escaped}"')
        changes.save()
    except Exception:
        changes.save()
        print(f"Installation stopped. Backups and recovery manifest are in {layout.state}.\nRun ./uninstall.sh with the same --prefix, or --state-dir {shlex.quote(str(layout.state))}, to undo applied changes.", file=sys.stderr)
        raise
    refresh_caches(layout)
    print(f"\nInstalled Kate Terminal Theme.\nLaunch: {layout.bin / 'kate'}\nBackups: {layout.state / 'backups'}\nUninstall: {layout.bin / 'kate-terminal-uninstall'}")
    print("Fully quit any older Kate windows before launching the updated setup.")
    if args.with_tmux:
        print(f"Reload tmux from its terminal: tmux source-file {shlex.quote(str(layout.tmux))}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install", "uninstall"])
    parser.add_argument("--prefix", type=Path, help="Install an isolated preview under this directory (config/data/cache/state/bin)")
    parser.add_argument("--state-dir", type=Path, help="Use this exact installation-state directory")
    parser.add_argument("--with-tmux", action="store_true", help="Also install the optional tmux palette and add one managed include")
    parser.add_argument("--no-icon", action="store_true", help="Keep the existing Kate application icon")
    parser.add_argument("--check", action="store_true", help="Show prerequisites/target directories without building or installing")
    parser.add_argument("--force", action="store_true", help="On uninstall, also restore files or settings modified since installation")
    args = parser.parse_args()
    layout = Layout(args)
    try:
        if sys.platform != "linux":
            raise RuntimeError("This installer targets Linux with Qt 6/KDE Frameworks 6 Kate.")
        if args.action == "install":
            install(args, layout)
        else:
            if not (layout.state / "install.json").exists():
                print(f"No installation manifest at {layout.state}; nothing to uninstall.")
                return
            changes = Changes(layout.state)
            skipped = changes.uninstall(args.force)
            refresh_caches(layout)
            print("Uninstall finished. Original backups remain in " + str(layout.state / "backups"))
            if skipped:
                print("Kept changes made after installation:")
                for entry in skipped:
                    print("  " + entry)
                print("The manifest remains so you can review backups or retry with --force.")
    except (OSError, RuntimeError, ValueError, ET.ParseError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
