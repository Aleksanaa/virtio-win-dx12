#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds yttrium's Venus Vulkan driver (libvulkan_virtio.dll) for the guest from the pinned Mesa fork
# (third_party/virtio-win-mesa) with icd/patches/*.patch applied, cross-compiled with the dev shell's MinGW GCC.
#
#   icd/build.sh [build-dir]      (default build/icd; run inside nix develop)
#
# The submodule stays pristine: the build compiles an exported copy with the patches applied.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build/icd}
mesa=$root/third_party/virtio-win-mesa
src=$out/src

mkdir -p "$out"
stamp=$(git -C "$mesa" rev-parse HEAD)
for p in "$root"/icd/patches/*.patch; do
    stamp="$stamp $(sha256sum "$p" | cut -d' ' -f1)"
done
if [ ! -f "$src/.stamp" ] || [ "$(cat "$src/.stamp")" != "$stamp" ]; then
    rm -rf "$src" "$out/meson"
    mkdir -p "$src"
    git -C "$mesa" archive HEAD | tar -x -C "$src"
    for p in "$root"/icd/patches/*.patch; do
        echo "icd: applying $(basename "$p")"
        (cd "$src" && patch -p1 --forward --quiet < "$p")
    done
    echo "$stamp" > "$src/.stamp"
fi

# GCC from nixpkgs uses the mcfgthread model; link it and the GCC runtime statically, as the UMD does.
# d3dkmthk.h and friends, which MinGW lacks, from the SDK after MinGW's own headers (as for the UMD).
sdk_shared=$root/refs/winkits/sdk/c/Include/10.0.26100.0/shared
cat > "$out/cross.ini" <<EOF
[binaries]
c = 'x86_64-w64-mingw32-gcc'
cpp = 'x86_64-w64-mingw32-g++'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'

[built-in options]
c_args = ['-isystem', '${VGD_MINGW_MCFG_INCLUDE}', '-idirafter', '$sdk_shared']
cpp_args = ['-isystem', '${VGD_MINGW_MCFG_INCLUDE}']
c_link_args = ['-L${VGD_MINGW_MCFG_LIB}', '-static', '-static-libgcc']
cpp_link_args = ['-L${VGD_MINGW_MCFG_LIB}', '-static', '-static-libgcc', '-static-libstdc++']

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

if [ ! -f "$out/meson/build.ninja" ]; then
    meson setup "$out/meson" "$src" --cross-file "$out/cross.ini" \
        --buildtype=release -Db_ndebug=true -Dc_std=c11 \
        -Dplatforms=windows -Dvulkan-drivers=virtio -Dgallium-drivers= -Dgallium-yttrium=true \
        -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
        -Dllvm=disabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled \
        -Dlibunwind=disabled -Dbuild-tests=false -Dvideo-codecs= -Dmicrosoft-clc=disabled
fi
ninja -C "$out/meson" src/virtio/vulkan/libvulkan_virtio.dll
# Installed next to vgd3d12.dll under the name the UMD looks for.
cp "$out/meson/src/virtio/vulkan/libvulkan_virtio.dll" "$out/vgd_vulkan_virtio.dll"
echo "icd: $out/vgd_vulkan_virtio.dll"
