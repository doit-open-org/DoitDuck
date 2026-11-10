#!/bin/sh
# What a CI job runs: the PC build (no SDK, no board) and the web page's translation check.
#
#   sh tools/ci.sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cmake -S "$ROOT/firmware/main_board/pc" -B "$ROOT/build/ci" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build/ci" -j"$(nproc)"
python3 "$ROOT/tools/i18n_check.py"
