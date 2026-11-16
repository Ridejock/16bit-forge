// SPDX-License-Identifier: MIT
// Plain copy of the input. With the "nearest" sampler this is the classic sharp-pixel look,
// with "linear" a plain bilinear stretch.
#include "filter_common.hlsli"

float4 main(float2 texCoord : TEXCOORD0) : SV_Target0
{
    return float4(SampleSource(texCoord), 1.0f);
}
