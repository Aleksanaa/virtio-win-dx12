// SPDX-License-Identifier: MIT
// Shaders for the depth/stencil and MSAA tests (d3dprobe --depth). Rebuild with build.sh.

cbuffer Depth : register(b0)   // root constant, vertex shader
{
    float z;
};

cbuffer Color : register(b1)   // root constants, pixel shader
{
    float4 color;
};

// One triangle over the whole target, at depth z.
float4 vs_flat(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), z, 1);
}

float4 ps_flat() : SV_Target
{
    return color;
}
