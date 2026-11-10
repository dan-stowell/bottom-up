#!/usr/bin/env python3
"""where.py PROG.strand ADDR  -- map a 'runtime check failed at 0x..' address to its source line"""
import bisect, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'boot'))
import strand
src = open(sys.argv[1], 'rb').read()
m = strand.compile_(src, True)[1]
k = bisect.bisect([x[0] for x in m], int(sys.argv[2], 16)) - 1
print('token', m[k][2], 'on line', m[k][1], ':', src.split(b'\n')[m[k][1] - 1][:160].decode('latin1'))
