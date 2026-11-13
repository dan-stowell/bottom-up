#!/usr/bin/env python3
"""objstat.py: what a link's inputs are made of. Walks every file of an importcfg.link (Go archives or bare
objects), finds the Go objects and sums the size of each block of the object format (cmd/internal/goobj).
   link/objstat.py <importcfg.link>      prints "key value" lines (bytes unless the key says count)"""
import struct, sys, os
BLK = ["autolib", "pkgidx", "file", "symdef", "hashed64def", "hasheddef", "nonpkgdef", "nonpkgref", "refflags",
       "hash64", "hash", "relocidx", "auxidx", "dataidx", "reloc", "aux", "data", "refname", "end"]
tot = {b: 0 for b in BLK}; tot.update(files=0, file_bytes=0, objects=0, goobj_bytes=0, strings=0, pkgdef=0)
def obj(b):
    if not b.startswith(b"go object "): return
    k = b.find(b"\n!\n")
    if k < 0: return
    b = memoryview(b)[k + 3:]
    if bytes(b[:8]) != b"\x00go120ld": return
    offs = struct.unpack_from("<%dI" % len(BLK), b, 20)
    tot["objects"] += 1; tot["goobj_bytes"] += offs[-1]
    tot["strings"] += offs[0] - (20 + 4 * len(BLK))
    for i in range(len(BLK) - 1): tot[BLK[i]] += offs[i + 1] - offs[i]
for line in open(sys.argv[1]):
    if not line.startswith("packagefile "): continue
    f = line.split("=", 1)[1].strip()
    b = open(f, "rb").read(); tot["files"] += 1; tot["file_bytes"] += len(b)
    if not b.startswith(b"!<arch>\n"): obj(b); continue
    off = 8
    while off + 60 <= len(b):
        name = b[off:off + 16].strip(); size = int(b[off + 48:off + 58]); body = b[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)
        if name.rstrip(b"/") == b"__.PKGDEF": tot["pkgdef"] += size; continue
        obj(body)
tot["nsym_slots"] = sum(tot[k] for k in ("symdef", "hashed64def", "hasheddef", "nonpkgdef", "nonpkgref")) // 21
tot["nreloc"] = tot["reloc"] // 23; tot["naux"] = tot["aux"] // 9
for k, v in tot.items():
    if k != "end": print(k, v)
