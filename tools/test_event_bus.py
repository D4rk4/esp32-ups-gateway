#!/usr/bin/env python3
"""Test the real EventBus with an in-memory queue boundary, not real FreeRTOS."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    build = ROOT / 'build' / ('event-bus-sanitize' if args.sanitize else 'event-bus')
    temporary = build / 'tmp'
    temporary.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment['TMPDIR'] = str(temporary)
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O1', '-g']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    includes = ['tests/platform/fakes', 'src', 'lib/GatewayCore/src', 'lib/UpsCore/src']
    executable = build / 'test_event_bus'
    subprocess.run([os.environ.get('CXX', 'g++'), *flags,
                    *['-I' + str(ROOT / p) for p in includes],
                    str(ROOT / 'tests/platform/test_event_bus.cpp'),
                    str(ROOT / 'src/event_bus.cpp'),
                    str(ROOT / 'lib/GatewayCore/src/events.cpp'),
                    '-o', str(executable)], cwd=ROOT, env=environment, check=True)
    subprocess.run([str(executable)], cwd=ROOT, env=environment, check=True)

if __name__ == '__main__':
    main()
