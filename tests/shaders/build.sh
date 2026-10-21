#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compiles the test shaders with dxc (signed DXIL containers) into C++ headers, one per .hlsl file. Needs dxc on
# PATH, e.g.
#   nix shell nixpkgs#directx-shader-compiler -c tests/shaders/build.sh
set -euo pipefail
cd "$(dirname "$0")"
. ../../core/shaders/dxil_header.sh

build triangle.hlsl vs_corner:vs_6_0 vs_buffer:vs_6_0 ps_color:ps_6_0
build textured.hlsl vs_fullscreen:vs_6_0 ps_textured:ps_6_0
build depth.hlsl vs_flat:vs_6_0 ps_flat:ps_6_0
build compute.hlsl cs_fill:cs_6_0
build indirect.hlsl cs_count:cs_6_0
