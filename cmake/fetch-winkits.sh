#!/bin/sh
# SPDX-License-Identifier: MIT
# Fetches the WDK and SDK headers (and the WDK's kernel-mode x64 libraries) the builds need from Microsoft's NuGet
# packages, Microsoft.Windows.WDK.x64 and Microsoft.Windows.SDK.CPP, and extracts them into refs/winkits/{wdk,sdk}.
#
#   cmake/fetch-winkits.sh [dir]      (default refs/winkits)
set -eu

version=10.0.26100.6584
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/refs/winkits}

fetch() {  # package, destination, paths to extract
    package=$1 dest=$2
    shift 2
    if [ -d "$dest/c" ]; then
        echo "winkits: $dest exists, skipping $package"
        return
    fi
    tmp=$(mktemp -d)
    echo "winkits: downloading $package $version"
    curl -fsSL -o "$tmp/pkg.zip" "https://api.nuget.org/v3-flatcontainer/$package/$version/$package.$version.nupkg"
    mkdir -p "$dest"
    unzip -q "$tmp/pkg.zip" "$@" -d "$dest"
    rm "$tmp/pkg.zip"
    rmdir "$tmp"
}

fetch microsoft.windows.wdk.x64 "$out/wdk" 'c/Include/*' 'c/Lib/10.0.26100.0/km/x64/*'
fetch microsoft.windows.sdk.cpp "$out/sdk" 'c/Include/*'
