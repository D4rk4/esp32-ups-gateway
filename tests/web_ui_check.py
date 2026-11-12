#!/usr/bin/env python3
"""Compatibility entry point for the v0.2 non-browser UI contract check.

The previous v0.1 browser script used obsolete selectors and authentication.
This entry point does NOT launch a browser or claim visual validation.
"""
from pathlib import Path
import subprocess

if __name__ == '__main__':
    root=Path(__file__).resolve().parents[1]
    print('Running v0.2 production-JavaScript contract checks; not browser or hardware validation.', flush=True)
    raise SystemExit(subprocess.call(['node',str(root/'tests/ui_fixture_contract_check.js')],cwd=root))
