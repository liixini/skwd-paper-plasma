#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

python3 -m unittest discover -s "$root/scripts/tests"
SKWD_PLASMA_BUILD_DIR=$temporary/build \
    "$root/scripts/package-stage.sh" "$temporary/stage"
