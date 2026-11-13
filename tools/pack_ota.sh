#!/bin/sh
# Upgrade packages for the web page (Settings -> Upgrade), each with a version.json saying what it is.
#
#   sh tools/pack_ota.sh app                                # doitduck, web/, robots/, sounds/ from firmware/main_board/out,
#                                                           # at DOITDUCK_VERSION (main/app/api.h)
#   sh tools/pack_ota.sh models 1.1 models/walk.bin ...     # these models, each at version 1.1
#   -> build/doitduck-app-<version>.tar.gz, build/doitduck-models-<version>.tar.gz
set -eu

DUCK="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$DUCK/firmware/main_board/out"
TMP="$DUCK/build/ota"
rm -rf "$TMP"
mkdir -p "$TMP"

KIND="${1:-}"
case "$KIND" in
app)
    VER=$(sed -n 's/^#define DOITDUCK_VERSION "\(.*\)"/\1/p' "$DUCK/firmware/main_board/main/app/api.h")
    cp -r "$OUT/doitduck" "$OUT/web" "$OUT/robots" "$OUT/sounds" "$TMP/"
    printf '{"kind": "app", "version": "%s"}\n' "$VER" > "$TMP/version.json"
    ;;
models)
    [ $# -ge 3 ] || { echo "usage: sh tools/pack_ota.sh models <version> <model file>..." >&2; exit 1; }
    VER=$2
    shift 2
    mkdir "$TMP/models"
    list=
    for f in "$@"; do
        cp "$f" "$TMP/models/"
        list="$list${list:+, }\"$(basename "$f")\": \"$VER\""
    done
    printf '{"kind": "models", "models": {%s}}\n' "$list" > "$TMP/version.json"
    ;;
*)
    echo "usage: sh tools/pack_ota.sh app | models <version> <model file>..." >&2
    exit 1
    ;;
esac

PKG="$DUCK/build/doitduck-$KIND-$VER.tar.gz"
(cd "$TMP" && tar czf "$PKG" *)
rm -rf "$TMP"
echo "pack_ota: $PKG"
