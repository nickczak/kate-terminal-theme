#!/bin/sh
set -eu
if ! command -v python3 >/dev/null 2>&1; then
    printf '%s\n' 'Python 3 is required. Install python3 with your package manager first.' >&2
    exit 1
fi
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$repo_dir/scripts/manage.py" uninstall "$@"
