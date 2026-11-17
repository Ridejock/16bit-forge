// SPDX-License-Identifier: MIT
// ScaleFX pass 4 by Sp00kyFox, ported to HLSL for SDL_GPU from libretro slang-shaders,
// edge-smoothing/scalefx/shaders/scalefx-pass4.slang. The algorithm is unchanged; GLSL idioms
// are spelled in HLSL (textureOffset -> Sample with an offset, mod -> fmod on non-negative
// values, bvec logic as scalar bools) and the #pragma parameters are @param parameters.
// Preset: source 3x, nearest, "input original" (the frame; "refpass" in the original).
// Original copyright and license (MIT), kept as it requires:
/*
	ScaleFX - Pass 4
	by Sp00kyFox, 2017-03-01

	ScaleFX is an edge interpolation algorithm specialized in pixel art. It was
	originally intended as an improvement upon Scale3x but became a new filter in
	its own right.
	ScaleFX interpolates edges up to level 6 and makes smooth transitions between
	different slopes. The filtered picture will only consist of colours present
	in the original.

	Pass 4 outputs subpixels based on previously calculated tags.

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

Texture2D<float4> Original : register(t1, space2);
SamplerState OriginalSampler : register(s1, space2);

// extract corners
float4 loadCrn(float4 x) { return floor(fmod(x * 80.0f + 0.5f, 9.0f)); }
// extract mids
float4 loadMid(float4 x) { return floor(fmod(x * 8.888888f + 0.055555f, 9.0f)); }

float4 main(float2 texCoord : TEXCOORD0) : SV_Target0
{
    /*  grid        corners     mids

          B         x   y         x
        D E F                   w   y
          H         w   z         z
    */

    // read data
    const float4 E = Source.SampleLevel(SourceSampler, texCoord, 0.0f);

    // extract data
    const float4 crn = loadCrn(E);
    const float4 mid = loadMid(E);

    // determine subpixel
    const float2 fp = floor(3.0f * frac(texCoord * SourceSize.xy));
    const float sp = fp.y == 0.0f ? (fp.x == 0.0f ? crn.x : fp.x == 1.0f ? mid.x : crn.y)
                   : (fp.y == 1.0f ? (fp.x == 0.0f ? mid.w : fp.x == 1.0f ? 0.0f : mid.y)
                                   : (fp.x == 0.0f ? crn.w : fp.x == 1.0f ? mid.z : crn.z));

    // output coordinate - 0 = E, 1 = D, 2 = D0, 3 = F, 4 = F0, 5 = B, 6 = B0, 7 = H, 8 = H0
    const float2 res = sp == 0.0f ? float2(0, 0) : sp == 1.0f ? float2(-1, 0) : sp == 2.0f ? float2(-2, 0)
                     : sp == 3.0f ? float2(1, 0) : sp == 4.0f ? float2(2, 0) : sp == 5.0f ? float2(0, -1)
                     : sp == 6.0f ? float2(0, -2) : sp == 7.0f ? float2(0, 1) : float2(0, 2);

    return Original.SampleLevel(OriginalSampler, texCoord + res * SourceSize.zw, 0.0f);
}
