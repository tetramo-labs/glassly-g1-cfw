#!/usr/bin/env bash
# Reproducible local build only. No Bluetooth or device writes.
set -euo pipefail
cd "$(dirname "$0")"
if [[ ! -x .venv/bin/python ]]; then
  "${G1_PYTHON:-python3}" -m venv .venv
fi
.venv/bin/python -c 'import sys; assert sys.version_info >= (3, 11), "Python 3.11+ is required"'
if ! .venv/bin/python - <<'PY'
from importlib.metadata import version
from pathlib import Path
for line in Path('requirements-cfw.txt').read_text().splitlines():
    name, expected = line.split('==')
    assert version(name) == expected, name
PY
then
  .venv/bin/python -m pip install -r requirements-cfw.txt
fi
.venv/bin/python build_cfw.py "$@"
