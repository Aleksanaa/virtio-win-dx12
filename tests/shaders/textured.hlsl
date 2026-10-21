// SPDX-License-Identifier: MIT
// Shaders for the descriptor tests (core/tests/descriptors.cpp, d3dprobe --textured). Rebuild with build.sh.
//
// The pixel shader reads every kind of binding the root signature can give it:
//   table 0 (CBV/SRV/UAV heap): b0 a constant buffer, t0 a texture, t1 a structured buffer
//   table 1 (sampler heap):     s0
//   static sampler:             s1
//   root CBV:                   b1

cbuffer Scale : register(b0) { float4 scale; };
cbuffer Bias : register(b1) { float4 bias; };
Texture2D<float4> tex : register(t0);
StructuredBuffer<float4> offsets : register(t1);
SamplerState heap_sampler : register(s0);
SamplerState static_sampler : register(s1);

// One triangle over the whole target.
float4 vs_fullscreen(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// Left half through the heap sampler, right half through the static one; both point-sample the 4x4 texture.
float4 ps_textured(float4 pos : SV_Position) : SV_Target
{
    const float2 uv = pos.xy / 64.0;
    const float4 texel = pos.x < 32 ? tex.Sample(heap_sampler, uv) : tex.Sample(static_sampler, uv);
    return texel * scale + offsets[1] + bias;
}
