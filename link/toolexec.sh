#!/usr/bin/env bash
# A -toolexec wrapper that substitutes hlink for the linker and runs every other tool as it is:
#   go build -toolexec=link/toolexec.sh ...       (HLINK names the binary; default build/hlink)
# hlink accepts the go command's link line (-o, -importcfg, -buildid, -buildmode=exe, -s, -w, -extld, -X).
# HLINK_HUGETLB=1: hlink links into a temporary file on hugetlbfs (HUGETLB_DIR, default /dev/hugepages), so that
# it creates its output there and lays it out for it, and the result is then copied to the path the go command
# asked for (which is not on hugetlbfs: the go command copies its link output around with write(), which
# hugetlbfs does not support). The copy is byte for byte the image hlink wrote on hugetlbfs.
tool=$1; shift
if [ "$(basename "$tool")" = link ] && [ "${1:-}" != "-V=full" ]; then
  here=$(cd "$(dirname "$0")/.." && pwd)
  hlink=${HLINK:-$here/build/hlink}
  [ -n "${HLINK_LOG:-}" ] && echo "$hlink $*" >> "$HLINK_LOG"
  if [ "${HLINK_HUGETLB:-}" = 1 ]; then
    args=("$@"); dest=""
    for ((i = 0; i < ${#args[@]}; i++)); do
      case ${args[i]} in
      -o) dest=${args[i+1]}; tmp=${HUGETLB_DIR:-/dev/hugepages}/hlink-toolexec.$$; args[i+1]=$tmp ;;
      -o=*) dest=${args[i]#-o=}; tmp=${HUGETLB_DIR:-/dev/hugepages}/hlink-toolexec.$$; args[i]=-o=$tmp ;;
      esac
    done
    [ -n "$dest" ] || { echo "toolexec.sh: HLINK_HUGETLB=1 needs -o" >&2; exit 2; }
    trap 'rm -f "$tmp"' EXIT
    prlimit --as=$((16 << 30)) "$hlink" "${args[@]}" || exit $?
    cp "$tmp" "$dest"
    exit $?
  fi
  exec prlimit --as=$((16 << 30)) "$hlink" "$@"
fi
exec "$tool" "$@"
