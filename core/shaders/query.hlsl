// SPDX-License-Identifier: MIT
// Fixes up query results ResolveQueryData has copied into the destination buffer (core/src/queries.cpp), where
// D3D12's answer differs from Vulkan's. Rebuild with build.sh.

cbuffer Params : register(b0)   // root constants
{
    uint count;   // results
    uint mode;    // 0: binary occlusion (0 or 1); 1: timestamps times num / den
    uint2 num;    // low, high
    uint2 den;
};

RWByteAddressBuffer results : register(u0);   // root UAV at the first result, 8 bytes each

[numthreads(64, 1, 1)]
void cs_query_fixup(uint id : SV_DispatchThreadID)
{
    if (id >= count)
        return;
    const uint2 v = results.Load2(id * 8);
    if (mode == 0) {
        results.Store2(id * 8, uint2((v.x | v.y) != 0 ? 1 : 0, 0));
        return;
    }
    // t * num / den without overflowing t * num: num * den fits 64 bits (queries.cpp makes sure).
    const uint64_t t = v.x | (uint64_t(v.y) << 32);
    const uint64_t n = num.x | (uint64_t(num.y) << 32), d = den.x | (uint64_t(den.y) << 32);
    const uint64_t r = t / d * n + t % d * n / d;
    results.Store2(id * 8, uint2(uint(r), uint(r >> 32)));
}
