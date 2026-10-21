#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compiles core's internal shaders into C++ headers, one per .hlsl file. Needs dxc on PATH, e.g.
#   nix shell nixpkgs#directx-shader-compiler -c core/shaders/build.sh
set -euo pipefail
cd "$(dirname "$0")"
. ./dxil_header.sh

build query.hlsl cs_query_fixup:cs_6_0
