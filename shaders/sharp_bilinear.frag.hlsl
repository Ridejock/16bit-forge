// SPDX-License-Identifier: MIT
// Sharp bilinear: nearest-neighbor up to the largest whole multiple of the input that fits the
// output, then bilinear for the fractional rest. Pixels stay crisp at any window size without
// the uneven pixel widths of plain nearest. Needs the "linear" sampler.
// Same idea as libretro's sharp-bilinear (Themaister, public domain); written from scratch here.
#include "filter_common.hlsli"

float4 main(float2 texCoord : TEXCOORD0) : SV_Target0
{
    const float2 texel = texCoord * SourceSize.xy;
    const float2 prescale = max(floor(OutputSize.xy * SourceSize.zw), 1.0f);
    const float2 center = frac(texel) - 0.5f;
    // Inside each source pixel, only the outer 1/(2 * prescale) band is blended with its neighbor.
    const float2 range = 0.5f - 0.5f / prescale;
    const float2 f = (center - clamp(center, -range, range)) * prescale + 0.5f;
    return float4(SampleSource((floor(texel) + f) * SourceSize.zw), 1.0f);
}
