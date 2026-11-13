#!/usr/bin/env bash
# Regenerate link/gotables.h: build link/gen/mktables.go inside the writable Go tree (it imports cmd/internal
# and internal packages) and run it.   link/gen/gen.sh
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
dst=${SB_GOROOT:-/home/exedev/targets/goroot-sb}
d=$dst/src/cmd/hlinkgen${HLINK_TAG:-}
mkdir -p "$d" "$here/../build"
cp "$here/gen/mktables.go" "$d/"
bin=$here/../build/hlink-mktables
(cd "$dst/src" && GOROOT="$dst" GOTOOLCHAIN=local GOFLAGS= GOEXPERIMENT= "$dst/bin/go" build -o "$bin" "cmd/hlinkgen${HLINK_TAG:-}")
GOEXPERIMENT= "$bin" > "$here/gotables.h"
echo "wrote $here/gotables.h"
