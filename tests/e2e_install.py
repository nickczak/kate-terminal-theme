#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build, install twice, and uninstall in a temporary prefix, including spaces."""
import json
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="kate theme e2e ") as name:
    prefix = Path(name) / "preview"
    config = prefix / "config/katerc"
    config.parent.mkdir(parents=True)
    config.write_text("[Private]\nMarker=preserve-me\n[General]\nShow Menu Bar=false\n")
    wrapper = prefix / "bin/kate"
    wrapper.parent.mkdir(parents=True)
    wrapper.write_text("#!/bin/sh\n# original launcher\n")
    wrapper.chmod(0o700)
    tmux = prefix / "tmux.conf"
    tmux.write_text("set -g mouse on\n")
    for _ in range(2):
        subprocess.run([str(repo / "install.sh"), "--prefix", str(prefix), "--with-tmux"], check=True)
    manifest = json.loads((prefix / "state/kate-terminal-theme/install.json").read_text())
    assert manifest["files"]
    assert config.read_text().count("kateterminalex=true") == 1
    assert "Marker=preserve-me" in config.read_text()
    assert tmux.read_text().count("# >>> kate-terminal-theme >>>") == 1
    qss = (prefix / "config/kate-terminal.qss").read_text()
    assert '@TREE_ASSET_DIR@' not in qss and 'url("' in qss
    assert (prefix / "data/kate-terminal-theme/plugins/kf6/ktexteditor/kateterminalex.so").is_file()
    config.write_text(config.read_text() + "\n[UserAddition]\nKeep=true\n")
    # Use the installed uninstaller: the original clone is not needed at runtime.
    subprocess.run([str(prefix / "bin/kate-terminal-uninstall")], check=True)
    assert "Marker=preserve-me" in config.read_text()
    assert "Show Menu Bar=false" in config.read_text()
    assert "Keep=true" in config.read_text()
    assert "kateterminalex=true" not in config.read_text()
    assert wrapper.read_text() == "#!/bin/sh\n# original launcher\n"
    assert wrapper.stat().st_mode & 0o777 == 0o700
    assert tmux.read_text().strip() == "set -g mouse on"
    assert not (prefix / "state/kate-terminal-theme/install.json").exists()
    assert not (prefix / "data/kate-terminal-theme/plugins/kf6/ktexteditor/kateterminalex.so").exists()
    print("PASS: build/install/update/uninstall; paths with spaces; original settings, launcher, and tmux preserved")
