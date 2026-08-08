#!/usr/bin/env bash
# Clone the SP-1 reference firmwares next to this script. Contents are
# gitignored: they are read-only references for transplanted code and the
# line numbers the plan cites.
set -euo pipefail
cd "$(dirname "$0")"
clone() {
  local url=$1 dir=$2
  if [ -d "$dir/.git" ]; then git -C "$dir" pull --ff-only; else git clone "$url" "$dir"; fi
}
clone https://github.com/chattock/sp1-tape-looper.git sp1-tape-looper
clone https://github.com/ericlewis/sp1-midi.git       sp1-midi
clone https://github.com/timknapen/SP-1-dev.git       SP-1-dev
clone https://github.com/timknapen/SP-1-dev.wiki.git  SP-1-dev-wiki
echo "references ready in $(pwd)"
