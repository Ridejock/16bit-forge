// SPDX-License-Identifier: MIT
// ScaleFX pass 0 by Sp00kyFox, ported to HLSL for SDL_GPU from libretro slang-shaders,
// edge-smoothing/scalefx/shaders/scalefx-pass0.slang. The algorithm is unchanged; GLSL idioms
// are spelled in HLSL (textureOffset -> Sample with an offset, mod -> fmod on non-negative
// values, bvec logic as scalar bools) and the #pragma parameters are @param parameters.
// Preset: source 1x, nearest, float. Output: colour distances to 4 neighbours.
// Original copyright and license (MIT), kept as it requires:
/*
	ScaleFX - Pass 0
	by Sp00kyFox, 2017-03-01

	ScaleFX is an edge interpolation algorithm specialized in pixel art. It was
	originally intended as an improvement upon Scale3x but became a new filter in
	its own right.
	ScaleFX interpolates edges up to level 6 and makes smooth transitions between
	different slopes. The filtered picture will only consist of colours present
	in the original.

	Pass 0 prepares metric data for the next pass.

	Copyright (c) 2016 Sp00kyFox - ScaleFX@web.de

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/
#include "filter_common.hlsli"

// Reference: http://www.compuphase.com/cmetric.htm
float dist(float3 A, float3 B)
{
    const float r = 0.5f * (A.r + B.r);
    const float3 d = A - B;
    const float3 c = float3(2.0f + r, 4.0f, 3.0f - r);
    return sqrt(dot(c * d, d)) / 3.0f;
}

float4 main(float2 texCoord : TEXCOORD0) : SV_Target0
{
    /*  grid        metric

        A B C       x y z
          E F         o w
    */
#define TEX(x, y) Source.SampleLevel(SourceSampler, texCoord, 0.0f, int2(x, y))

    const float3 A = TEX(-1, -1).rgb;
    const float3 B = TEX( 0, -1).rgb;
    const float3 C = TEX( 1, -1).rgb;
    const float3 E = TEX( 0,  0).rgb;
    const float3 F = TEX( 1,  0).rgb;

    return float4(dist(E, A), dist(E, B), dist(E, C), dist(E, F));
}
