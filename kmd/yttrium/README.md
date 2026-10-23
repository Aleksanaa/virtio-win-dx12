# yttrium kernel driver (vendored)

The parts of arehnman's yttrium-virtio-gpu that `viogpu3d.sys` is built from, imported unchanged from
https://github.com/arehnman/yttrium-virtio-gpu at tag `yttrium-experimental-2026-09-27` (b99d951a) in the import commit.
Our changes are ordinary commits on top; `git diff <import commit> -- kmd/yttrium` is the patch set we offer upstream.

Kept: `viogpu/common`, `viogpu/shared` (headers), `viogpu/viogpu3d` (sources), `viogpu/viogpudo/viogpudo.h`, `VirtIO`
(without the WDF variant), `build/VirtIOTestCert.*` (yttrium's test-signing certificate) and the license files.
Dropped: MSBuild projects, batch scripts, packaging tools and the other virtio-win drivers. Removed after the import,
as the build never used them: the INX files and resource script (our driver package is `pkg/`).

Added later from the same tag, unchanged: `viogpu/viogpusc` (the `vgpusrv` service) and `viogpu/viogpuap` (the helper it
starts in each user session, which applies the resolution the host asks for, so the guest follows the viewer window),
with `shared/log.cpp` and `shared/pipe.cpp`, which they use. Without their MSBuild projects and batch scripts.

License, as upstream, per file: the virtio-win parts (`VirtIO`, `viogpu/common`, `viogpum.h`, ...) are BSD-3-Clause
(see the LICENSE files); yttrium's own viogpu3d files carry `SPDX-License-Identifier: MPL-2.0` headers. Our changes
to MPL files stay under MPL-2.0.
