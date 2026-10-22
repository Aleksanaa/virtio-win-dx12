#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds what DXVK's d3d11.dll is linked from, without linking it: its static libraries and its object files, which
# go into one archive for vgd3d11.dll. d3d11_main.cpp's object is left out: it holds the exported D3D11CreateDevice
# family (and DXVK's logger instance, which vgd3d11 defines itself).
#   dxvk-objects.sh <ninja> <ar> <meson build dir> <output archive> <static libraries...>
set -eu
ninja=$1 ar=$2 build=$3 out=$4
shift 4
case $out in
    /*.a) ;;
    *) echo "dxvk-objects.sh: output must be an absolute .a path, got '$out'" >&2; exit 1 ;;
esac

objects=$("$ninja" -C "$build" -t query src/d3d11/d3d11.dll |
    sed -n 's/^    \(src\/d3d11\/d3d11\.dll\.p\/.*\.obj\)$/\1/p' | grep -v '/d3d11_main\.cpp\.obj$')
[ -n "$objects" ] || { echo "dxvk-objects.sh: no objects found for d3d11.dll" >&2; exit 1; }
libs=
for lib in "$@"; do
    libs="$libs ${lib#"$build"/}"
done
# shellcheck disable=SC2086 # word splitting intended: lists of build paths without spaces
"$ninja" -C "$build" $objects $libs

cd "$build"
rm -f "$out.tmp"
# shellcheck disable=SC2086
"$ar" rcs "$out.tmp" $objects
mv -f "$out.tmp" "$out"
