// SPDX-License-Identifier: MIT
// Shared by every filter pass (*.frag.hlsl). GpuBackend binds the pass input to slot 0 with the
// sampler named by the preset (nearest or linear) and pushes this block as uniform buffer 0.
// SDL_GPU fragment resources: textures/samplers in space2, uniform buffers in space3.
//
// Extra inputs (the preset's "input <name>" options) go to slots 1, 2, ... in that order, with
// the same sampler; declare them in the shader like Source, e.g.
//     Texture2D<float4> Original : register(t1, space2);
//     SamplerState OriginalSampler : register(s1, space2);
// The shader must declare exactly as many textures as the pass gets, or the preset won't load.
//
// Parameters: a line "// @param <name> <default>  description" (at the start of the line, no
// semicolons) declares the next parameter slot, Parameter(0), Parameter(1), ... in that order.
// A preset's "param = <name>, <value>" line overrides the default. Up to 16 per shader.
#ifndef FILTER_COMMON_HLSLI
#define FILTER_COMMON_HLSLI

Texture2D<float4> Source : register(t0, space2);
SamplerState SourceSampler : register(s0, space2);

cbuffer FilterUniforms : register(b0, space3)
{
    float4 SourceSize;   // pass input:   w, h, 1/w, 1/h
    float4 OutputSize;   // pass output:  w, h, 1/w, 1/h (the viewport for the last pass)
    float4 OriginalSize; // emulator frame: w, h, 1/w, 1/h
    uint FrameCount;     // presented frames, for effects that alternate over time
    uint3 Padding;
    float4 Parameters[4];
};

float Parameter(uint index)
{
    return Parameters[index / 4][index % 4];
}

float3 SampleSource(float2 uv)
{
    return Source.Sample(SourceSampler, uv).rgb;
}

#endif
