#!/usr/bin/env bash
# secdiff catches bugs: link hello with the stock linker and the linker under test (link/secdiff.sh), then
# mutate the outputs the way a buggy linker would and check that secdiff reports each mutation:
#   1. a relocated value flipped in the hlink binary      -> "hlink: value is not the formula ..."
#   2. the same site flipped in the stock binary           -> "stock value is not the formula ..."
#   3. a symbol dropped from the hlink map                 -> "only in stock: text", relocations to it
#   4. a pcsp byte corrupted in the hlink binary           -> "pcsp table differs"
#   5. a pointer bit cleared in hlink's data GC program    -> "pointer bit differs"
# Environment: LINKER (default build/hlink, else build/fclinkmap), HLINK_OUT (default /tmp/hlink-try).
set -e
repo=$(cd "$(dirname "$0")/../.." && pwd)
if [ -z "${LINKER:-}" ]; then
	LINKER=$repo/build/hlink
	[ -x "$LINKER" ] || LINKER=$repo/build/fclinkmap
fi
[ -x "$repo/build/fclinkmap" ] || "$repo/link/secdiff/build.sh" fclinkmap >&2
export LINKER
out=${HLINK_OUT:-/tmp/hlink-try}/secdiff/hello
"$repo/link/secdiff.sh" "$repo/link/progs/hello" > /dev/null 2>&1 || true # (baseline; may report real differences)
sd=$repo/build/secdiff
mut=$out/mut
mkdir -p "$mut"
base=$mut/base.txt
"$sd" -n 50 "$out/hlink.map" "$out/hlink" "$out/stock.map" "$out/stock" > "$base" || true

# flip BIN ADDR: xor the byte at virtual address ADDR of ELF file BIN (in place)
flip() {
	python3 -I - "$1" "$2" "${3:-255}" <<'EOF'
import struct, sys
path, addr, mask = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
b = bytearray(open(path, 'rb').read())
phoff, = struct.unpack_from('<Q', b, 32)
phnum, = struct.unpack_from('<H', b, 56)
for i in range(phnum):
    typ, flags, off, vaddr, paddr, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', b, phoff + 56 * i)
    if typ == 1 and vaddr <= addr < vaddr + filesz:
        b[off + addr - vaddr] ^= mask
        open(path, 'wb').write(b)
        sys.exit(0)
sys.exit('address %#x not in the file' % addr)
EOF
}

fail=0
expect() { # name, file with secdiff's output, kind text
	if grep -qF "$3" "$2" && ! grep -qF "$3" "$base"; then
		echo "PASS $1: reported as \"$3\""
		grep -A1 -F "$3" "$2" | sed -n 2p | cut -c1-200
	else
		echo "FAIL $1: \"$3\" not reported"
		fail=1
	fi
}

# 1, 2: the first call relocation in main.main
read -r ha hs < <(awk '$1=="S" && $5=="main.main" {print $2, $3; exit}' "$out/hlink.map")
sa=$(awk '$1=="S" && $5=="main.main" {print $2; exit}' "$out/stock.map")
site=$(awk -v lo=$((16#$ha)) -v hi=$((16#$ha + hs)) '$1=="R" && $4==7 {a=strtonum("0x" $2); if (a>=lo && a<hi) {print $2; exit}}' "$out/hlink.map")
[ -n "$site" ] || { echo "selftest: no call relocation in main.main"; exit 2; }
cp "$out/hlink" "$mut/hlink.1"
flip "$mut/hlink.1" $((16#$site))
"$sd" "$out/hlink.map" "$mut/hlink.1" "$out/stock.map" "$out/stock" > "$mut/1.txt" || true
expect "flipped relocation (hlink binary, main.main+$((16#$site - 16#$ha)))" "$mut/1.txt" "hlink: value is not the formula with hlink's own addresses"
cp "$out/stock" "$mut/stock.2"
flip "$mut/stock.2" $((16#$sa + 16#$site - 16#$ha))
"$sd" "$out/hlink.map" "$out/hlink" "$out/stock.map" "$mut/stock.2" > "$mut/2.txt" || true
expect "flipped relocation (stock binary)" "$mut/2.txt" "stock value is not the formula with the stock addresses"

# 3: drop fmt.Fprintln from hlink's map
grep -v ' text fmt.Fprintln$' "$out/hlink.map" > "$mut/hlink.3.map"
"$sd" -n 50 "$mut/hlink.3.map" "$out/hlink" "$out/stock.map" "$out/stock" > "$mut/3.txt" || true
expect "dropped symbol fmt.Fprintln (hlink map)" "$mut/3.txt" "only in stock: text"
expect "dropped symbol fmt.Fprintln (relocations to it)" "$mut/3.txt" "hlink: target not in hlink's map"

# 4: a pcsp byte of main.main
pa=$("$sd" -locate pcsp:main.main "$out/hlink.map" "$out/hlink" "$out/stock.map" "$out/stock")
cp "$out/hlink" "$mut/hlink.4"
flip "$mut/hlink.4" $((pa + 1)) 1
"$sd" "$out/hlink.map" "$mut/hlink.4" "$out/stock.map" "$out/stock" > "$mut/4.txt" || true
expect "corrupted pcsp byte of main.main" "$mut/4.txt" "pcsp table differs"

# 5: the GC program of data: clear the first literal bit run's first byte (a pointer word somewhere)
gd=$(awk '$1=="S" && $5=="runtime.gcdata" {print $2; exit}' "$out/hlink.map")
cp "$out/hlink" "$mut/hlink.5"
python3 -I - "$mut/hlink.5" $((16#$gd)) <<'EOF'
import struct, sys
path, addr = sys.argv[1], int(sys.argv[2])
b = bytearray(open(path, 'rb').read())
phoff, = struct.unpack_from('<Q', b, 32)
phnum, = struct.unpack_from('<H', b, 56)
for i in range(phnum):
    typ, flags, off, vaddr, paddr, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', b, phoff + 56 * i)
    if typ == 1 and vaddr <= addr < vaddr + filesz:
        p = off + addr - vaddr
        # walk the program to the first literal with a set bit and clear its lowest set bit
        while True:
            inst = b[p]; p += 1
            n = inst & 0x7f
            if inst & 0x80 == 0:
                if n == 0:
                    sys.exit('no pointer bit')
                for k in range((n + 7) // 8):
                    if b[p + k]:
                        b[p + k] &= b[p + k] - 1
                        open(path, 'wb').write(b)
                        sys.exit(0)
                p += (n + 7) // 8
                continue
            if n == 0:
                while b[p] & 0x80: p += 1
                p += 1
            while b[p] & 0x80: p += 1
            p += 1
sys.exit('runtime.gcdata not in the file')
EOF
"$sd" "$out/hlink.map" "$mut/hlink.5" "$out/stock.map" "$out/stock" > "$mut/5.txt" || true
expect "cleared pointer bit (hlink gcdata)" "$mut/5.txt" "pointer bit differs"

[ $fail = 0 ] && echo "selftest: every mutation reported"
exit $fail
