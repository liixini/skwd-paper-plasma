#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=${1:-}
output=${2:-}

if ! printf '%s\n' "$version" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$'; then
    echo "usage: scripts/package-source.sh VERSION OUTPUT" >&2
    exit 2
fi
if [ -z "$output" ]; then
    echo "usage: scripts/package-source.sh VERSION OUTPUT" >&2
    exit 2
fi
case "$output" in
    /*) ;;
    *) output=$PWD/$output ;;
esac
if [ -e "$output" ]; then
    echo "refusing to overwrite source archive: $output" >&2
    exit 1
fi
if [ -n "$(git -C "$root" status --porcelain=v1 --untracked-files=all)" ]; then
    echo "source checkout must be clean" >&2
    exit 1
fi

mkdir -p "$(dirname -- "$output")"
temporary=$output.tmp
trap 'rm -f "$temporary"' EXIT HUP INT TERM
git -C "$root" archive --format=tar --prefix="skwd-paper-plasma-$version/" HEAD \
    | xz -T0 -9e > "$temporary"
mv "$temporary" "$output"
