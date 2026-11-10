#!/usr/bin/env python3
"""check_probe.py PROBE_BINARY -- every "does it execute" probe must contain the instruction it is
named for. The compiler is free to turn an intrinsic into something else (AUDIT.md, defect 3: the AVX2
probe had become a scalar add and the ADX probe an ordinary adc), so this is checked on the disassembly."""
import re, subprocess, sys
WANT = {'sse2': r'\bpaddd\b', 'ssse3': r'\bpshufb\b', 'sse41': r'\bpmulld\b', 'sse42': r'\bcrc32', 'popcnt': r'\bpopcnt\b', 'pclmul': r'pclmul',
        'aes': r'\baesenc\b', 'sha': r'\bsha1msg1\b', 'avx': r'\bvaddps\b.*ymm', 'avx2': r'\bvpaddd\b.*ymm', 'fma': r'\bvfmadd', 'f16c': r'\bvcvtps2ph\b',
        'bmi1': r'\b(tzcnt|blsr)\b', 'bmi2': r'\b(pext|pdep|bzhi)\b', 'lzcnt': r'\blzcnt\b', 'adx': r'\b(adcx|adox)\b', 'movbe': r'\bmovbe\b',
        '512f': r'zmm', '512f_k': r'\bvpcmp\w*\b.*%k', '512f_compress': r'\bvpcompressd\b', '512f_gather': r'\bvpgatherdd\b', '512f_scatter': r'\bvpscatterdd\b',
        '512dq': r'\b(vpmullq|vpmovd2m)\b', '512bw': r'\b(vpcmp\w*b|vpaddb)\b.*zmm', '512vl': r'\bvpcmp\w+\b.*%[xy]mm\d+,%k\d', '512cd': r'\b(vpconflict|vplzcnt)', '512ifma': r'\bvpmadd52',
        '512vbmi': r'\bvperm(b|i2b|t2b)\b', '512vbmi2': r'\b(vpcompress[bw]|vpexpand[bw]|vpsh[lr]d)', '512vnni': r'\bvpdp', '512bitalg': r'\b(vpopcnt[bw]|vpshufbitqmb)\b',
        '512vpopcnt': r'\bvpopcnt[dq]\b', '512bf16': r'\b(vdpbf16|vcvtne)', 'gfni': r'gf2p8', 'vaes': r'\bvaes', 'vpclmul': r'\bvpclmul', 'fp16': r'\bv\w+(ph|sh)\b',
        'vp2i': r'\bvp2intersect', 'avxvnni': r'\bvpdp', 'sse4a': r'\b(extrq|insertq)\b', 'amx': r'\b(tile\w*|ldtilecfg|sttilecfg)\b', 'apx': r'%r(1[6-9]|2\d|3[01])[dwb]?\b'}
dis = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', sys.argv[1]], text=True)
bad = 0; seen = set(); unchecked = []
for name, body in re.findall(r'<p_(\w+)>:\n(.*?)(?=\n\n|\Z)', dis, re.S):
    seen.add(name)
    if name not in WANT: print('no expectation for', name); bad += 1
    elif WANT[name] is None: unchecked.append(name)
    elif not re.search(WANT[name], body): print(f'p_{name}: no instruction matching {WANT[name]}'); bad += 1
for name in sorted(set(WANT) - seen):       # a binary without the probes must not pass (second audit, finding 5)
    print(f'p_{name}: probe missing from the binary'); bad += 1
if unchecked: print('not checked:', ' '.join(unchecked))
print('probe check:', 'ok' if not bad else f'{bad} problems'); sys.exit(1 if bad else 0)
