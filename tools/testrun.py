#!/usr/bin/env python3
"""A timed test run on Windows: starts the game through run.py, and after --seconds ends the
whole process tree (the port has no watchdog on Windows, so a hung game would stay open).

    python tools/testrun.py --game <folder> --seconds 20 --log out/run.log [--trace sceNp,scePad]

Prints the log's tail without the GPU's start-up lines. The exit code is the game's, or 124
when it had to be ended.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

PORT = Path(__file__).resolve().parent.parent
NOISE = re.compile(r'Render\.Vulkan|not fully supported|missing features|^\s*$|TransferSrc|Storage'
                   r'|direct allocation|mapped direct|^Runtime:   |semaphore id|event flag id')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--game', required=True)
    p.add_argument('--seconds', type=float, default=20)
    p.add_argument('--log', type=Path, default=PORT / 'out' / 'testrun.log')
    p.add_argument('--trace', default='', help='SF_IMPORT_TRACE prefixes')
    p.add_argument('--tail', type=int, default=40)
    p.add_argument('--env', action='append', default=[], help='NAME=value, repeatable')
    a = p.parse_args()
    env = {**os.environ, 'SF_GAME_DIR': a.game, 'PYTHONIOENCODING': 'utf-8'}
    if a.trace:
        env['SF_IMPORT_TRACE'] = a.trace
    for item in a.env:
        name, _, value = item.partition('=')
        env[name] = value
    a.log.parent.mkdir(parents=True, exist_ok=True)
    with a.log.open('wb') as log:
        process = subprocess.Popen([sys.executable, str(PORT / 'run.py')], stdin=subprocess.DEVNULL,
                                   stdout=log, stderr=subprocess.STDOUT, env=env, cwd=PORT)
        try:
            code = process.wait(timeout=a.seconds)
        except subprocess.TimeoutExpired:
            code = 124
        finally:
            # Always: run.py may have ended while the game it started is still open.
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], capture_output=True)
            subprocess.run(['taskkill', '/F', '/IM', 'sixfret.exe'], capture_output=True)
            process.wait()
    lines = a.log.read_text(encoding='utf-8', errors='replace').splitlines()
    kept = [line for line in lines if not NOISE.search(line)]
    print(f'exit={code} lines={len(lines)} threads={sum("guest thread " in line for line in lines)}')
    print('\n'.join(kept[-a.tail:]))
    return code


if __name__ == '__main__':
    sys.exit(main())
