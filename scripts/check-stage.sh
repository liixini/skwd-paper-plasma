#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
stage=${1:-}

if [ -z "$stage" ] || [ ! -d "$stage" ] || [ -L "$stage" ]; then
    echo "usage: scripts/check-stage.sh STAGE" >&2
    exit 1
fi

actual=$(mktemp)
trap 'rm -f "$actual"' EXIT HUP INT TERM

find "$stage" -type f -printf '%P\n' | LC_ALL=C sort > "$actual"
if ! cmp -s "$root/packaging/manifest.txt" "$actual"; then
    echo "Plasma package stage differs from packaging/manifest.txt" >&2
    diff -u "$root/packaging/manifest.txt" "$actual" >&2 || true
    exit 1
fi

if find "$stage" \( -type l -o \( ! -type d ! -type f \) \) -print -quit | grep -q .; then
    echo "Plasma package stage contains a non-regular entry" >&2
    exit 1
fi
