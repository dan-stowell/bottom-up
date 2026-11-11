#!/usr/bin/env python3
"""golex_fuzz.py [ROUNDS] [SEED] -- the Go lexer against go/scanner on adversarial one-line snippets.

Each round writes a few thousand random lines, built from the pieces where a lexer's token boundaries are
decided (digits, dots, exponent and prefix letters, operators, quotes, comments). `golex lines` keeps
the lines go/scanner accepts without error and gives their tokens; the Strand lexer must give the same
bytes for the kept file, on one core and on all. Found by the fifth audit: x.0, 0a, 1ii, ...0."""
import os, random, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, 'build')
ATOMS = ['0', '1', '7', '9', '00', '12', '.', '..', '...', 'e', 'E', 'p', 'P', 'x', 'X', 'b', 'B', 'o', 'O', 'i', 'a', 'f', 'F', 'g', '_', 'z9',
         '+', '-', '*', '/', '%', '&', '|', '^', '<', '>', '=', '!', ':', '~', '(', ')', '[', ']', '{', '}', ',', ';', ' ', ' ', '\t',
         '0x', '0b', '0o', '0X1F', '1e5', '1e+5', '.5', '5.', '1i', '0x1p-2', "'a'", "'\\''", '"s"', '"\\""', '`r`', '/*c*/', '/**/', '//c', '\\',
         'if', 'go', 'for', 'func', 'return', 'break', 'x', 'é', '<-', '&^', '<<=', '++', '--', ':=', '"', "'", '`', '/*', '*/']
def line(r):
    n = r.choice((1, 2, 2, 3, 3, 4, 5, 6, 8, 12))
    return ''.join(r.choice(ATOMS) for _ in range(n))
def decode(b):
    n = len(b) // 5
    return [(int.from_bytes(b[4 * i:4 * i + 4], 'little'), b[4 * n + i]) for i in range(n)]
def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    r = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    lexer = os.path.join(B, 'golex_strand'); ref = os.path.join(B, 'golex')
    cpu0 = str(min(os.sched_getaffinity(0)))
    kept = 0
    for rd in range(rounds):
        src = os.path.join(B, 'gofuzz.in'); ok = os.path.join(B, 'gofuzz.ok')
        with open(src, 'w') as f:
            for _ in range(4000 if rd % 4 else 60000): f.write(line(r) + '\n')      # every fourth round is big enough for the pool
        want = subprocess.run([ref, 'lines', src, ok], stdout=subprocess.PIPE, check=True).stdout
        for cmd in ([lexer], ['taskset', '-c', cpu0, lexer]):
            p = subprocess.run(cmd, stdin=open(ok, 'rb'), stdout=subprocess.PIPE)
            if p.returncode or p.stdout != want:
                text = open(ok, 'rb').read(); a, b = decode(want), decode(p.stdout)
                i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
                off = a[i][0] if i < len(a) else len(text) - 1
                s = text.rfind(b'\n', 0, off) + 1; e = text.find(b'\n', s)
                ex = [(o - s, k) for o, k in a if s <= o <= e]; got = [(o - s, k) for o, k in b if s <= o <= e]
                print(f'FAIL golex fuzz round {rd} ({" ".join(cmd)}), status {p.returncode}: {text[s:e]!r}\n  go/scanner {ex}\n  strand     {got}')
                sys.exit(1)
        kept += want and len(open(ok, 'rb').read().split(b'\n')) - 1
    print(f'PASS golex fuzz ({rounds} rounds, {kept} lines go/scanner accepts)')
main()
