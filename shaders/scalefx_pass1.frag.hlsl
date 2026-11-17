// SPDX-License-Identifier: MIT
// ScaleFX pass 1 by Sp00kyFox, ported to HLSL for SDL_GPU from libretro slang-shaders,
// edge-smoothing/scalefx/shaders/scalefx-pass1.slang. The algorithm is unchanged; GLSL idioms
// are spelled in HLSL (textureOffset -> Sample with an offset, mod -> fmod on non-negative
// values, bvec logic as scalar bools) and the #pragma parameters are @param parameters.
// Preset: source 1x, nearest, float. Input: pass 0's metrics.
// Original copyright and license (MIT), kept as it requires:
/*
	ScaleFX - Pass 1
	by Sp00kyFox, 2017-03-01

	ScaleFX is an edge interpolation algorithm specialized in pixel art. It was
	originally intended as an improvement upon Scale3x but became a new filter in
	its own right.
	ScaleFX interpolates edges up to level 6 and makes smooth transitions between
	different slopes. The filtered picture will only consist of colours present
	in the original.

	Pass 1 calculates the strength of interpolation candidates.

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

// @param sfx_threshold 0.50   ScaleFX threshold (0.01 - 1)
// @param sfx_aa 1.0           ScaleFX filter AA (0 or 1)
#define SFX_CLR Parameter(0)
#define SFX_SAA Parameter(1)

// corner strength
float str(float d, float2 a, float2 b)
{
    const float diff = a.x - a.y;
    const float wght1 = max(SFX_CLR - d, 0.0f) / SFX_CLR;
    const float wght2 = saturate((1.0f - d) + (min(a.x, b.x) + a.x > min(a.y, b.y) + a.y ? diff : -diff));
    return (SFX_SAA == 1.0f || 2.0f * d < a.x + a.y) ? (wght1 * wght2) * (a.x * a.y) : 0.0f;
}

float4 main(float2 texCoord : TEXCOORD0) : SV_Target0
{
    /*  grid        metric      pattern

        A B         x y z       x y
        D E F         o w       w z
        G H I
    */
#define TEX(x, y) Source.SampleLevel(SourceSampler, texCoord, 0.0f, int2(x, y))

    // metric data
    const float4 A = TEX(-1, -1), B = TEX( 0, -1);
    const float4 D = TEX(-1,  0), E = TEX( 0,  0), F = TEX( 1,  0);
    const float4 G = TEX(-1,  1), H = TEX( 0,  1), I = TEX( 1,  1);

    // corner strength
    float4 res;
    res.x = str(D.z, float2(D.w, E.y), float2(A.w, D.y));
    res.y = str(F.x, float2(E.w, E.y), float2(B.w, F.y));
    res.z = str(H.z, float2(E.w, H.y), float2(H.w, I.y));
    res.w = str(H.x, float2(D.w, H.y), float2(G.w, G.y));
    return res;
}
