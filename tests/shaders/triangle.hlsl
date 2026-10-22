// SPDX-License-Identifier: MIT
// Shaders for the triangle tests (d3dprobe --triangle). Rebuild with build.sh.

cbuffer Color : register(b0)   // root constants
{
    float4 color;
};

// The top-left corner of the target: (-1, 1), (0, 1), (-1, 0), clockwise on screen. Mirrored or with the wrong
// winding it would land elsewhere or be culled.
float4 vs_corner(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 0.0 : -1.0, id == 2 ? 0.0 : 1.0, 0.0, 1.0);
}

// Positions from a vertex buffer.
float4 vs_buffer(float2 pos : POSITION) : SV_Position
{
    return float4(pos, 0.0, 1.0);
}

float4 ps_color() : SV_Target
{
    return color;
}
