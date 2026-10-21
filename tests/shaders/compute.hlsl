// SPDX-License-Identifier: MIT
// Shaders for the compute tests (core/tests/compute.cpp, d3dprobe --compute). Rebuild with build.sh.
//
// Root signature: b0 root constants, u0 a root UAV (structured buffer), table {u1 typed buffer, u2 texture}.

cbuffer Params : register(b0)
{
    uint mult;
    uint add;
};

RWStructuredBuffer<uint> structured : register(u0);
RWBuffer<uint> typed : register(u1);
RWTexture2D<float4> image : register(u2);

// 256 threads: element i of each buffer, and texel (i % 16, i / 16) of the image's top-left 16x16.
[numthreads(64, 1, 1)]
void cs_fill(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    structured[i] = i * mult + add;
    typed[i] = i + 1000;
    const uint2 xy = uint2(i % 16, i / 16);
    image[xy] = float4(xy.x * 17, xy.y * 17, 51, 255) / 255.0;
}
