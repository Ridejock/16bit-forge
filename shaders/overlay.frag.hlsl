// SPDX-License-Identifier: MIT
// On-screen messages: the overlay image (straight alpha), faded by Opacity. Not a filter pass:
// GpuBackend draws it with alpha blending over the finished picture, after the filter chain,
// into a viewport that is a whole multiple of the image size (nearest sampler).

Texture2D<float4> Overlay : register(t0, space2);
SamplerState OverlaySampler : register(s0, space2);

cbuffer OverlayUniforms : register(b0, space3)
{
    float Opacity;
    float3 Padding;
};

float4 main(float2 texCoord : TEXCOORD0) : SV_Target0
{
    const float4 color = Overlay.Sample(OverlaySampler, texCoord);
    return float4(color.rgb, color.a * Opacity);
}
