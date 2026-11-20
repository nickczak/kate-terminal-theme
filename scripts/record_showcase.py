#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Record a real Kate walkthrough in an isolated profile, then encode the GIF."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap

repo = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, default=repo / "showcase-desktop.gif")
parser.add_argument("--frames-dir", type=Path, help="Keep raw PNG frames in this empty directory")
args = parser.parse_args()
for program in ["ffmpeg", "cmake", "python3"]:
    if not shutil.which(program):
        raise SystemExit(f"Missing {program}; install it before recording.")

build = repo / ".showcase-build"
subprocess.run(["cmake", "-S", str(repo / "plugin"), "-B", str(build), "-DBUILD_SHOWCASE_RECORDER=ON", "-DBUILD_TESTING=OFF", "-DBUILD_KATE_INTEGRATION_TESTS=OFF"], check=True)
subprocess.run(["cmake", "--build", str(build), "--parallel", "3"], check=True)

with tempfile.TemporaryDirectory(prefix="kate-theme-showcase-") as directory:
    root = Path(directory)
    prefix = root / "profile"
    frames = args.frames_dir.resolve() if args.frames_dir else root / "frames"
    frames.mkdir(parents=True, exist_ok=True)
    if any(frames.glob("frame-*.png")):
        raise SystemExit(f"Frame directory is not empty: {frames}")
    subprocess.run([str(repo / "install.sh"), "--prefix", str(prefix)], check=True)
    plugins = root / "recorder-plugins/kf6/ktexteditor"
    plugins.mkdir(parents=True)
    shutil.copy2(build / "showcase_recorder.so", plugins / "showcase_recorder.so")
    session = prefix / "data/kate/anonymous.katesession"
    session.write_text(session.read_text().replace("[Kate Plugins]\n", "[Kate Plugins]\nshowcase_recorder=true\n"))
    project = root / "workspace"
    (project / "src").mkdir(parents=True)
    (project / "tests").mkdir()
    (project / ".kateproject").write_text(json.dumps({"name": "kate-terminal-theme", "files": [{"directory": ".", "recursive": True}]}))
    (project / "src/palette.py").write_text(textwrap.dedent('''\
        """The GitHub Dark palette, with a pure-black canvas."""

        COLORS = {
            "background": "#000000",
            "foreground": "#e1e4e8",
            "blue": "#79b8ff",
            "purple": "#b392f0",
            "green": "#85e89d",
            "coral": "#f97583",
        }


        def paint(text: str, color: str) -> str:
            """Use the same palette in the embedded terminal."""
            channels = [int(color[i:i + 2], 16) for i in (1, 3, 5)]
            red, green, blue = channels
            return f"\\033[38;2;{red};{green};{blue}m{text}\\033[0m"
        '''))
    (project / "src/navigation.py").write_text(textwrap.dedent('''\
        """Small, familiar motions for the Projects tree."""

        MOTIONS = {
            "j": "next item",
            "k": "previous item",
            "Enter": "open file or toggle folder",
            ":Ex": "focus Projects",
        }
        '''))
    (project / "main.py").write_text(textwrap.dedent('''\
        """A small workspace for Kate Terminal Theme."""

        from src.palette import COLORS, paint
        from src.navigation import MOTIONS


        def main() -> None:
            title = paint("Kate Terminal Theme", COLORS["blue"])
            print(f"\\n{title}\\n")

            features = [
                ("Pure-black editor and terminal", "green"),
                ("GitHub Dark syntax colors", "purple"),
                ("Vim motions in Projects", "blue"),
            ]
            for label, accent in features:
                print(paint(f"  + {label}", COLORS[accent]))

            print("\\n  :Ex  |  j / k  |  Enter\\n")


        if __name__ == "__main__":
            main()
        '''))
    (project / "tests/test_palette.py").write_text(textwrap.dedent('''\
        import unittest
        from src.palette import COLORS


        class PaletteTests(unittest.TestCase):
            def test_black_canvas(self):
                self.assertEqual(COLORS["background"], "#000000")

            def test_github_accents(self):
                self.assertEqual(COLORS["blue"], "#79b8ff")
                self.assertEqual(COLORS["purple"], "#b392f0")
        '''))
    (project / "README.md").write_text(textwrap.dedent('''\
        # Kate Terminal Theme

        GitHub colors. A pure-black canvas. Familiar Vim motions.

        ## A small workflow

        - Type `:Ex` to focus Projects.
        - Move through files with `j` and `k`.
        - Press Enter to open a file or expand a folder.
        - Run your checks in the matching terminal panel.

        Projects hides when you return to the editor.

        ## Run the sample

        ```sh
        python3 -m unittest discover -s tests -v
        python3 main.py
        ```
        '''))
    shell = shutil.which("sh")
    profile = prefix / "data/konsole/KateGitHubBlack.profile"
    profile.write_text(profile.read_text().replace("[General]\n", f"[General]\nCommand={shell}\nArguments={shell}\nDirectory={project}\nShowTerminalSizeHint=false\n"))
    shell_rc = root / "empty-shell-rc"
    shell_rc.write_text("")
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_SCALE_FACTOR="1", QT_FONT_DPI="96",
               QT_PLUGIN_PATH=str(root / "recorder-plugins"),
               KATE_SHOWCASE_PROJECT=str(project), KATE_SHOWCASE_FRAMES=str(frames),
               SHELL=shell, ENV=str(shell_rc), HISTFILE="/dev/null",
               PS1="❯ ")
    subprocess.run([str(prefix / "bin/kate"), "--block", "--startanon", str(project / "main.py")], env=env, check=True, timeout=45)
    frame_list = sorted(frames.glob("frame-*.png"))
    if len(frame_list) < 200:
        raise SystemExit(f"Recording too short: {len(frame_list)} frames")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "warning", "-y", "-framerate", "12",
        "-i", str(frames / "frame-%05d.png"), "-filter_complex",
        "[0:v]split[frames][colors];[colors]palettegen=max_colors=256:stats_mode=diff[palette];"
        "[frames][palette]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle",
        "-loop", "0", "-gifflags", "+transdiff", str(output)
    ], check=True)
    shutil.copy2(frame_list[0], repo / "assets/showcase-poster.png")
    print(f"Saved {output} ({output.stat().st_size / 1024 / 1024:.2f} MiB, {len(frame_list)} frames)")
