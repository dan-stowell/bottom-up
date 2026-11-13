#!/usr/bin/env bash
# A -toolexec wrapper that substitutes hlink for the linker and runs every other tool as it is:
#   go build -toolexec=link/toolexec.sh ...       (HLINK names the binary; default build/hlink)
# hlink accepts the go command's link line (-o, -importcfg, -buildid, -buildmode=exe, -s, -w, -extld, -X).
tool=$1; shift
if [ "$(basename "$tool")" = link ] && [ "${1:-}" != "-V=full" ]; then
  here=$(cd "$(dirname "$0")/.." && pwd)
  hlink=${HLINK:-$here/build/hlink}
  [ -n "${HLINK_LOG:-}" ] && echo "$hlink $*" >> "$HLINK_LOG"
  exec prlimit --as=$((16 << 30)) "$hlink" "$@"
fi
exec "$tool" "$@"
