#!/bin/sh
# sh render.sh VERSION SHA256 OUT_DIR   - fills in the winget manifest templates
set -eu
[ $# -eq 3 ] || { echo "usage: $0 VERSION SHA256 OUT_DIR" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$3"
for f in "$HERE"/*.yaml; do
    sed -e "s/__VERSION__/$1/g" -e "s/__SHA256__/$2/g" -e "s/__DATE__/$(date -u +%F)/g" "$f" > "$3/$(basename "$f")"
done
