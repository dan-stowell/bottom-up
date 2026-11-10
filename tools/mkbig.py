#!/usr/bin/env python3
"""mkbig.py K -- a large *valid* Strand program on stdout: K copies of the compiler's own source.
Tee-defined names are single-assignment, so in copies 2..K each one gets the copy number appended."""
import os, re, sys
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
src = open(R + '/src/tables.strand').read() + open(R + '/src/compiler.strand').read()
tees = set(re.findall(r'(?:^|\s)[:%]([a-z_][A-Za-z0-9_]*)', src))
out = [src]
for k in range(2, int(sys.argv[1]) + 1):
    def ren(m):
        sig, name = m.group(1), m.group(2)
        return sig + name + ('_%d' % k if name in tees else '')
    lines = []
    for line in src.split('\n'):
        if line.startswith('#') or line.rstrip().endswith('=prelude') or '"' in line: lines.append(line); continue
        lines.append(' '.join(re.sub(r'^([>@?!&:%]?)([a-z_][A-Za-z0-9_]*)$', ren, t) for t in line.split(' ')))
    out.append('\n'.join(lines))
sys.stdout.write('\n'.join(out))
