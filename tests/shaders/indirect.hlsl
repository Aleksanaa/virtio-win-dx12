// SPDX-License-Identifier: MIT
// Shaders for the ExecuteIndirect tests (d3dprobe --indirect). Rebuild with build.sh.

RWByteAddressBuffer counter : register(u0);   // root UAV

// Counts its invocations.
[numthreads(64, 1, 1)]
void cs_count()
{
    counter.InterlockedAdd(0, 1);
}
