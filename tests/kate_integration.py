#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the installed package in a real, isolated, offscreen Kate window."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
build = repo / ".build"
subprocess.run(["cmake", "-S", str(repo / "plugin"), "-B", str(build), "-DBUILD_TESTING=ON", "-DBUILD_KATE_INTEGRATION_TESTS=ON"], check=True)
subprocess.run(["cmake", "--build", str(build), "--parallel", "3"], check=True)
with tempfile.TemporaryDirectory(prefix="kate real integration ") as name:
    root = Path(name)
    prefix = root / "installed setup"
    subprocess.run([str(repo / "install.sh"), "--prefix", str(prefix)], check=True)
    # Only the test probe is added separately. The actual feature plugin must
    # be discovered from the package installed by install.sh and its launcher.
    probe_plugins = root / "probe-plugins"
    probe = probe_plugins / "kf6/ktexteditor/libexprobe.so"
    probe.parent.mkdir(parents=True)
    shutil.copy2(build / "libexprobe.so", probe)
    session = prefix / "data/kate/anonymous.katesession"
    session.write_text(session.read_text().replace("[Kate Plugins]\n", "[Kate Plugins]\nlibexprobe=true\n"))
    project = root / "project"
    (project / "src").mkdir(parents=True)
    (project / "src/module.py").write_text('print("hello")\n')
    (project / "test.txt").write_text("Explorer test\n")
    (project / ".kateproject").write_text(json.dumps({"name": "Integration test", "files": [{"directory": ".", "recursive": True}]}))
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_PLUGIN_PATH=str(probe_plugins),
               KATE_TERMINAL_DIAGNOSTICS=str(root / "runtime.jsonl"))
    subprocess.run([str(prefix / "bin/kate"), "--block", "--startanon", str(project / "test.txt")], env=env, check=True, timeout=30)
    records = [json.loads(line) for line in (root / "runtime.jsonl").read_text().splitlines()]
    assert any(row["event"] == "plugin-view-created" and row["Ex_registered"] and row["ex_registered"] for row in records)
    assert any(row["event"] == "focus-projects-triggered" for row in records)
    assert 'url("' in (prefix / "config/kate-terminal.qss").read_text()
    subprocess.run([str(prefix / "bin/kate-terminal-uninstall")], check=True)
    assert not (prefix / "state/kate-terminal-theme/install.json").exists(), "Uninstall left unexpected modified settings"
    print("PASS: real Kate launched through the installed wrapper; installed plugin, terminal profile, Projects keys, Ex dispatch, and uninstall")
