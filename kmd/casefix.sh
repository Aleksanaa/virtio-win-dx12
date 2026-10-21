#!/bin/sh
# SPDX-License-Identifier: MIT
# usage: casefix.sh <out-dir> <include-dir>...
# Windows sources include headers by names whose case differs from the files (DriverSpecs.h -> driverspecs.h).
# For every #include name in the given directories that exists nowhere with that exact spelling but does exist
# with another case, links <out-dir>/<requested name> to the file.
out=$1; shift
mkdir -p "$out"
idx=$(mktemp)
for d in "$@"; do find "$d" -maxdepth 1 \( -type f -o -type l \); done | awk -F/ '{print tolower($NF) "\t" $0}' > "$idx"
for d in "$@"; do
    grep -rhoE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]+[>"]' "$d" \
        --include='*.h' --include='*.c' --include='*.cpp' --include='*.inl' 2>/dev/null
done | sed -E 's/.*[<"]([^>"]+)[>"].*/\1/' | grep -v / | sort -u | while read -r n; do
    found=0
    for d in "$@"; do [ -e "$d/$n" ] && found=1 && break; done
    [ $found = 1 ] && continue
    p=$(awk -F'\t' -v k="$(echo "$n" | tr 'A-Z' 'a-z')" '$1 == k { print $2; exit }' "$idx")
    [ -n "$p" ] && ln -sf "$p" "$out/$n"
done
rm -f "$idx"
