#!/usr/bin/env python3
"""Windows: opens the launcher, saves a picture of its window (only that window) and closes it.

    python tools/launcher_shot.py out/launcher.png [page]
"""
import ctypes
import ctypes.wintypes
import json
import subprocess
import sys
import time
from pathlib import Path

from PIL import ImageGrab

ROOT = Path(__file__).resolve().parent.parent
TITLE = 'Guitar Hero Live — Six Fret'


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'out' / 'launcher.png')
    if len(sys.argv) > 2:  # the launcher opens on the page it was last closed on
        (ROOT / 'resume.json').write_text(json.dumps({'page': sys.argv[2]}), encoding='utf-8')
    process = subprocess.Popen([sys.executable, str(ROOT / 'launcher' / 'sixfret_launcher.py')], cwd=ROOT)
    try:
        user32 = ctypes.windll.user32
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
        window = 0
        for _ in range(60):
            time.sleep(0.25)
            window = user32.FindWindowW(None, TITLE)
            if window:
                break
        if not window:
            raise SystemExit('launcher window not found')
        time.sleep(2.5)
        user32.SetForegroundWindow(window)
        time.sleep(0.5)
        rect = ctypes.wintypes.RECT()
        user32.GetWindowRect(window, ctypes.byref(rect))
        ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom)).save(out)
        print(out, rect.right - rect.left, 'x', rect.bottom - rect.top)
    finally:
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], capture_output=True)


if __name__ == '__main__':
    main()
