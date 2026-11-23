#!/usr/bin/env python3
"""Writes the game's trophy texts into the runtime and the launcher from tools/trophies.txt.

tools/trophies.txt has one trophy per line in PSN order (which is the trophy id):
name|description|grade. The texts inside trophy00.trp are encrypted, so the port carries its own.

    python tools/gen_trophies.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def rewrite(path, change):
    text = path.read_text(encoding='utf-8')
    newline = '\r\n' if '\r\n' in path.read_bytes().decode('utf-8') else '\n'
    path.write_text(change(text), encoding='utf-8', newline=newline)


def c_string(text):
    return text.replace('\\', '\\\\').replace('"', '\\"')


def main():
    rows = [line.split('|') for line in (ROOT / 'tools' / 'trophies.txt').read_text(encoding='utf-8').splitlines() if line]
    if any(len(r) != 3 or len(r[0]) >= 64 or len(r[1]) >= 160 for r in rows):
        raise SystemExit('trophies.txt: name|description|grade, name under 64 and description under 160 characters')

    def runtime(text):
        start = text.index('#define GAME_TROPHIES')
        end = text.index('};', start) + 2
        table = (f'#define GAME_TROPHIES {len(rows)}\nstatic const char *const game_trophies[GAME_TROPHIES][3]={{\n'
                 + ''.join(f'    {{"{c_string(n)}","{c_string(d)}","{g}"}}, /* {i} */\n' for i, (n, d, g) in enumerate(rows))
                 + '};')
        return text[:start] + table + text[end:]

    def launcher(text):
        start = text.index('TROPHIES = (')
        end = text.index('\n)\n', start) + 3
        table = 'TROPHIES = (\n' + ''.join(f'    ({n!r}, {d!r}, {g!r}),  # {i}\n' for i, (n, d, g) in enumerate(rows)) + ')\n'
        return text[:start] + table + text[end:]

    rewrite(ROOT / 'src' / 'runtime_services.c', runtime)
    rewrite(ROOT / 'scripts' / 'game_trophies.py', launcher)
    print(f'{len(rows)} trophies written')


if __name__ == '__main__':
    main()
