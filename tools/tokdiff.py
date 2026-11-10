#!/usr/bin/env python3
"""tokdiff.py SOURCE WANT GOT -- compare two token dumps (all offsets, then all kinds, as dwords) and
show the first difference with its source context."""
import struct, sys
src = open(sys.argv[1], 'rb').read()
def load(p):
    d = open(p, 'rb').read(); n = len(d) // 8
    return list(struct.unpack('<%dI' % n, d[:4 * n])), list(struct.unpack('<%dI' % n, d[4 * n:8 * n])), len(d)
(wo, wk, wl), (go_, gk, gl) = load(sys.argv[2]), load(sys.argv[3])
print('want', len(wo), 'tokens;', 'got', len(go_), 'tokens', '(odd size!)' if gl % 8 else '')
for i in range(min(len(wo), len(go_))):
    if (wo[i], wk[i]) != (go_[i], gk[i]):
        lo = max(0, wo[i] - 60)
        print(f'first difference at token {i}: want (offset {wo[i]}, kind {wk[i]}) got (offset {go_[i]}, kind {gk[i]})')
        print('context:', src[lo:wo[i]].decode('latin1').replace('\n', '\\n') + ' >>> ' + src[wo[i]:wo[i] + 40].decode('latin1').replace('\n', '\\n'))
        print('want next:', [(o, k) for o, k in zip(wo[i:i + 6], wk[i:i + 6])]); print('got  next:', [(o, k) for o, k in zip(go_[i:i + 6], gk[i:i + 6])])
        sys.exit(1)
if len(wo) != len(go_): print('one stream is a prefix of the other'); sys.exit(1)
print('identical')
