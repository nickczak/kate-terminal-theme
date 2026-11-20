# Third-party assets

- `themes/GitHub Black.theme` derives from **GitHub Dark**, distributed with KDE Syntax Highlighting. Copyright 2020 GitHub Inc. and 2020 Waqar Ahmed. The theme retains GitHub's syntax palette, uses a pure-black background/gutter, and softens the current-line background to `#14171a`. It is MIT licensed; attribution is also retained in its metadata. See `LICENSES/MIT.txt` and [KDE Syntax Highlighting](https://invent.kde.org/frameworks/syntax-highlighting).
- `themes/TerminalBlack.colors` derives from **Breeze Dark** and retains its original author notices. It is LGPL-2.0-or-later licensed. See `LICENSES/LGPL-2.0-or-later.txt` and [KDE Breeze](https://invent.kde.org/plasma/breeze).
- `assets/kate-terminal-black.svg`, used in the README header, derives from the **KDE Breeze Kate application icon**. Copyright 2014 Uri Herrera and others. Its circular background and shadow are changed from blue to pure black. The artwork is LGPL-3.0-or-later licensed; the complete upstream artwork notice and license are in `LICENSES/Breeze-COPYING-ICONS`. The editable SVG source is included. See [KDE Breeze Icons](https://invent.kde.org/frameworks/breeze-icons).
- The installer still generates the application icon from the target machine's existing Kate SVG. It skips that customization if a compatible SVG is unavailable; `--no-icon` explicitly disables it.

The plugin, installer, tree symbols, stylesheet, and other original files are covered by the repository's MIT license unless a file states otherwise.
