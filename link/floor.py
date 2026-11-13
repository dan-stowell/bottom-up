#!/usr/bin/env python3
"""floor.py: regenerate FLOOR.md from the measured machine limits (hw/linklimits.out.txt), what each workload's
inputs are made of (link/results/<w>.objstat, from link/objstat.py) and the linker timings
(link/results/<w>.<linker>.tsv, from link/bench.sh).        link/floor.py [linker=hlink] > FLOOR.md

The floor of a phase is the larger of
  bytes:        bytes it must read / the DRAM read-stream rate + bytes it must write / the DRAM write-stream rate
                (8 threads; a copy benchmark counts each byte once but moves it twice, so it is not the divisor)
  instructions: instructions / (8 cores x measured instructions per second per core)
  random:       independent random accesses / the measured random-load rate for a table of that size (added to
                the byte time: both use the memory system), where the input format scatters what must be read
"Must" is what any linker of these inputs has to touch for that phase, not what ours touches (the model is in
PHASES below; change it there, with the reason). OS costs (opening the inputs, creating the output file, tearing
the address space down) are not in the hardware floor: they are their own rows, with the measured kernel limit
in the floor column, and they are in the totals' measured milliseconds, so the last column is honest."""
import sys, os, re, glob
here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def kv(path):
    d = {}
    for line in open(path):
        p = line.split()
        if len(p) == 2 and not line.startswith("#"): d[p[0]] = float(p[1])
    return d
hw = kv(os.path.join(here, "hw/linklimits.out.txt"))
linker = sys.argv[1] if len(sys.argv) > 1 else "hlink"
def top(k): return max(hw[k + "_4t"], hw[k + "_8t"])          # all cores (4 threads often already saturate)
READ, WRITE = top("read_dram"), top("write_dram")
INSTR = hw["instr_8t"]
RAND_L3 = top("rand_16m")
RAND_DRAM = top("rand_512m")                                  # independent loads from a table far bigger than the caches

def load_tsv(path):
    t, notes = {}, {}
    for line in open(path):
        if line.startswith("#"):
            m = re.match(r"# (\w+)\s+[\d.]+\s+[\d.]+\s+(.*)", line)
            if m: notes[m.group(1)] = m.group(2)
            m = re.match(r"# (output_bytes|measured) (.*)", line)
            if m: notes[m.group(1)] = m.group(2)
            continue
        j, ph, ms = line.split("\t"); t[(int(j), ph)] = float(ms)
    return t, notes
def num(note, pat, default=0):
    m = re.search(pat, note or "")
    return float(m.group(1)) if m else default

def phases(o, notes):
    """rows: (name, linker phases it covers, bytes in, bytes out, instructions, random L3 accesses,
    random DRAM accesses, why)"""
    slots = o["nsym_slots"]; SYM, REL, AUX = 21, 23, 9
    out = float(notes.get("output_bytes", 0))
    live = num(notes.get("deadcode"), r"(\d+) symbols live")
    lrel = num(notes.get("write"), r"(\d+) relocations applied")
    byname = o["nonpkgdef"] + o["nonpkgref"]
    names = o["strings"] * byname / (slots * SYM)          # the names of the symbols joined by name (average length)
    dc = notes.get("deadcode", "")
    flooded = num(dc, r"(\d+) symbols flooded", live)       # symbols whose relocations had to be read
    edges = num(dc, r"(\d+) relocations and", lrel) + num(dc, r"and (\d+) aux records read")
    hreached = num(dc, r"content-addressed: (\d+) reached")
    hashed = o["hashed64def"] + o["hasheddef"]               # bytes of content-addressed symbol records
    # resolve: by-name symbols and their names are read once; of the content-addressed ones only the reached need
    # their hash read (the linker says how many; before it did, all of them)
    hbytes = hreached * 16 if hreached else o["hash64"] + o["hash"]
    return [
        ("resolve", ["resolve"], (slots * SYM - (hashed if hreached else 0)) + hbytes + names, 4 * (byname / SYM + (hreached or hashed / SYM)), 20 * slots, 0, 0,
         "every by-name and package-indexed symbol record and by-name name read once; the hashes of the reached "
         "content-addressed symbols; one canonical id written per joined slot"),
        ("reach", ["inittasks", "deadcode"], num(dc, r"(\d+) relocations and", lrel) * REL + num(dc, r"and (\d+) aux records read") * AUX, live, 10 * edges, edges, flooded,
         "the relocations and aux records of the symbols flooded; one mark per edge (flags fit L3); and one "
         "random DRAM access per flooded symbol, whose records start anywhere in 0.8 GB of objects"),
        ("layout", ["layout"], live * SYM, 4 * live, 10 * live, 0, live,
         "sizes of the live symbols read (one random DRAM access each); one address written per live symbol"),
        ("write", ["create", "write", "finish"], out + lrel * REL, out, 2 * out, lrel, live,
         "every output byte copied once from the objects (pclntab and metadata included); live relocations read; "
         "one address lookup each; one random DRAM access per live symbol for where its bytes start"),
    ]

def os_rows(o, notes):
    out = float(notes.get("output_bytes", 0))
    touched = o["nsym_slots"] * 21 + o["hash"] + o["hash64"] + out   # input pages any linker has to fault in, roughly
    return [
        ("OS: open inputs", ["load"], o["files"] / max(hw["openmap_4t"], hw["openmap_8t"]) * 1e3,
         "%d files at %.0fk open+fstat+mmap+close per second" % (o["files"], max(hw["openmap_4t"], hw["openmap_8t"]) / 1e3)),
        ("OS: create output", [], out / 4096 / max(hw["filemap_write_disk_8t"], hw["filemap_write_disk_4t"]) * 1e3,
         "%.0f MB of new page-cache pages through a shared mapping at %.2fM pages/s (ext4; tmpfs would be %.1f ms, pwrite %.1f ms)" % (
             out / 1e6, max(hw["filemap_write_disk_8t"], hw["filemap_write_disk_4t"]) / 1e6,
             out / 4096 / max(hw["filemap_write_shm_8t"], hw["filemap_write_shm_4t"]) * 1e3, out / hw["pwrite_disk_1t"] * 1e3)),
        ("OS: map inputs, exit", ["exit"], touched / 4096 / hw["filemap_read_8t"] * 1e3 + touched / 4096 / hw["munmap_file"] * 1e3,
         "%.0f MB of input pages faulted in (%.1fM pages/s) and unmapped at exit (%.1fM pages/s, one thread)" % (
             touched / 1e6, hw["filemap_read_8t"] / 1e6, hw["munmap_file"] / 1e6)),
    ]

def mb(b): return "%.1f MB" % (b / 1e6) if b else "-"
print("# FLOOR: % of floor, per workload and phase\n")
print("Generated by `link/floor.py %s` (do not edit). Limits: `hw/linklimits.out.txt`; timings: `link/results/`.\n" % linker)
print("Machine limits used (8 cores): DRAM read stream %.1f GB/s, write stream %.1f GB/s (copy: %.1f GB/s of payload), "
      "%.0f G instructions/s (%.1f on 1 core), random loads: %.2f G/s into an L3-sized table, %.2f G/s into a 512 MB one.\n" % (
          READ / 1e9, WRITE / 1e9, top("copy_dram") / 1e9, INSTR / 1e9, hw["instr_1t"] / 1e9, RAND_L3 / 1e9, RAND_DRAM / 1e9))
for path in sorted(glob.glob(os.path.join(here, "link/results/*.%s.tsv" % linker))):
    w = os.path.basename(path).split(".")[0]
    o = kv(os.path.join(here, "link/results/%s.objstat" % w))
    t, notes = load_tsv(path)
    print("## %s (%s, %s)\n" % (w, linker, notes.get("measured", "")))
    print("| workload/phase | bytes in | bytes out | ms (1 core) | ms (2) | ms (4) | ms (8 cores) | floor ms (8 cores) | % of floor | binds |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    def ms(j, names): return sum(t.get((j, n), 0) for n in names)
    hwfloor = 0
    for name, cover, bin_, bout, instr, rnd, rdram, why in phases(o, notes):
        # streams and random accesses share the memory system: their times add; instructions run beside them
        mem = (bin_ / READ + bout / WRITE + rnd / RAND_L3 + rdram / RAND_DRAM) * 1e3
        f = {"memory": mem, "instructions": instr / INSTR * 1e3}
        bind = max(f, key=f.get); fl = f[bind]; hwfloor += fl
        m8 = ms(8, cover)
        print("| %s/%s | %s | %s | %.1f | %.1f | %.1f | %.1f | %.2f | %.1f%% | %s |" % (
            w, name, mb(bin_), mb(bout), ms(1, cover), ms(2, cover), ms(4, cover), m8, fl, 100 * fl / m8 if m8 else 0, bind))
    osfloor = 0
    for name, cover, fl, why in os_rows(o, notes):
        osfloor += fl
        def m(j):
            if cover == ["exit"]: return t.get((j, "process"), 0) - t.get((j, "total"), 0)
            return ms(j, cover)
        cells = ["%.1f" % m(j) if cover else "(in write)" for j in (1, 2, 4, 8)]
        pct = "%.1f%%" % (100 * fl / m(8)) if cover and m(8) else ""
        print("| %s/%s | | | %s | %s | %s | %s | %.2f | %s | kernel |" % (w, name, *cells, fl, pct))
    p = [t.get((j, "process"), 0) for j in (1, 2, 4, 8)]
    print("| **%s/total (process, warm)** | %s | %s | %.1f | %.1f | %.1f | **%.1f** | **%.2f** (hardware) | **%.1f%%** | |" % (
        w, mb(o["goobj_bytes"]), mb(float(notes.get("output_bytes", 0))), *p, hwfloor, 100 * hwfloor / p[3] if p[3] else 0))
    print("| %s/total against hardware + OS limits | | | | | | %.1f | %.2f | %.1f%% | |" % (w, p[3], hwfloor + osfloor, 100 * (hwfloor + osfloor) / p[3] if p[3] else 0))
    if (8, "cold") in t:
        cold_bytes = o["nsym_slots"] * 21 + o["hash"] + o["hash64"] + float(notes.get("output_bytes", 0))
        cf = cold_bytes / kv(os.path.join(here, "hw/limits.out.txt"))["disk_read_cold_bytes_per_s"] * 1e3
        print("| %s/total (process, cold) | | | | | | %.1f | %.2f (disk read of the %.0f MB touched) | %.1f%% | disk |" % (
            w, t[(8, "cold")], cf + hwfloor, cold_bytes / 1e6, 100 * (cf + hwfloor) / t[(8, "cold")]))
    print("\nWhat each floor counts:\n")
    for name, cover, bin_, bout, instr, rnd, rdram, why in phases(o, notes): print("- **%s**: %s." % (name, why))
    for name, cover, fl, why in os_rows(o, notes): print("- **%s**: %s." % (name, why))
    print()
