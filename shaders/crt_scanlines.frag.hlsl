// SPDX-License-Identifier: MIT
// Simple CRT look: a soft dark gap between the emulated scanlines plus a mild RGB aperture
// mask, with a small brightness lift to make up for the darkening. Needs the "linear" sampler
// and an output larger than the input (it is meant for the last pass, at the viewport size).
// The aperture-grille mask follows Emerald's Crt.frag.hlsl (MIT, Ervin Ashley).
#include "filter_common.hlsli"

static const float kScanlineStrength = 0.45f; // 0 = no gaps, 1 = black gaps
static const float kMaskStrength = 0.15f;     // 0 = no mask, 1 = pure RGB stripes
static const float kBrightness = 1.25f;

float4 main(float2 texCoord : TEXCOORD0, float4 position : SV_Position) : SV_Target0
{
    // Sample the vertical center of the emulated line, so each line is one flat band.
    const float row = texCoord.y * SourceSize.y;
    const float2 uv = float2(texCoord.x, (floor(row) + 0.5f) * SourceSize.w);
    float3 color = SampleSource(uv);

    // Bright in the middle of each line, darker towards its edges.
    const float offset = frac(row) - 0.5f;
    const float beam = exp(-offset * offset * 12.0f);
    color *= lerp(1.0f, beam, kScanlineStrength);

    // Aperture grille: every output pixel column favors one of red, green, blue.
    const int column = int(position.x) % 3;
    const float3 stripe = column == 0 ? float3(1, 0.4f, 0.4f)
                                      : (column == 1 ? float3(0.4f, 1, 0.4f) : float3(0.4f, 0.4f, 1));
    color *= lerp(float3(1, 1, 1), stripe * 1.5f, kMaskStrength);

    return float4(saturate(color * kBrightness), 1.0f);
}
