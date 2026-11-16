// SPDX-License-Identifier: MIT
// Shared by every filter pass (*.frag.hlsl). GpuBackend binds the pass input to slot 0 with the
// sampler named by the preset (nearest or linear) and pushes this block as uniform buffer 0.
// SDL_GPU fragment resources: textures/samplers in space2, uniform buffers in space3.
#ifndef FILTER_COMMON_HLSLI
#define FILTER_COMMON_HLSLI

Texture2D<float4> Source : register(t0, space2);
SamplerState SourceSampler : register(s0, space2);

cbuffer FilterUniforms : register(b0, space3)
{
    float4 SourceSize;   // pass input:   w, h, 1/w, 1/h
    float4 OutputSize;   // pass output:  w, h, 1/w, 1/h (the viewport for the last pass)
    float4 OriginalSize; // emulator frame: w, h, 1/w, 1/h
    uint FrameCount;
    uint3 Padding;
};

float3 SampleSource(float2 uv)
{
    return Source.Sample(SourceSampler, uv).rgb;
}

#endif
