#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
stage=${1:-$root/.skwd-package/stage}
build=${SKWD_PLASMA_BUILD_DIR:-$root/.skwd-package/build}

case "$stage" in
    /*) ;;
    *) stage=$PWD/$stage ;;
esac

if [ -e "$stage" ]; then
    if [ ! -d "$stage" ] || [ -L "$stage" ] || [ -n "$(find "$stage" -mindepth 1 -print -quit)" ]; then
        echo "refusing to overwrite non-empty package stage: $stage" >&2
        exit 1
    fi
else
    mkdir -p "$stage"
fi

cmake -S "$root" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$build" --parallel
DESTDIR=$stage cmake --install "$build"
"$root/scripts/check-stage.sh" "$stage"
