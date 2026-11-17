// SPDX-License-Identifier: MIT
// ScaleFX pass 2 by Sp00kyFox, ported to HLSL for SDL_GPU from libretro slang-shaders,
// edge-smoothing/scalefx/shaders/scalefx-pass2.slang. The algorithm is unchanged; GLSL idioms
// are spelled in HLSL (textureOffset -> Sample with an offset, mod -> fmod on non-negative
// values, bvec logic as scalar bools) and the #pragma parameters are @param parameters.
// Preset: source 1x, nearest, "input metric" (pass 0's output). Input: pass 1's strengths.
// Original copyright and license (MIT), kept as it requires:
/*
	ScaleFX - Pass 2
	by Sp00kyFox, 2017-03-01

	ScaleFX is an edge interpolation algorithm specialized in pixel art. It was
	originally intended as an improvement upon Scale3x but became a new filter in
	its own right.
	ScaleFX interpolates edges up to level 6 and makes smooth transitions between
	different slopes. The filtered picture will only consist of colours present
	in the original.

	Pass 2 resolves ambiguous configurations of corner candidates at pixel junctions.

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

Texture2D<float4> Metric : register(t1, space2);  // pass 0 ("scalefx_pass0" in the original)
SamplerState MetricSampler : register(s1, space2);

#define LE(x, y) (1.0f - step(y, x))
#define GE(x, y) (1.0f - step(x, y))
#define LEQ(x, y) step(x, y)
#define GEQ(x, y) step(y, x)
#define NOT(x) (1.0f - (x))

// corner dominance at junctions
float4 dom(float3 x, float3 y, float3 z, float3 w)
{
    return 2.0f * float4(x.y, y.y, z.y, w.y) - (float4(x.x, y.x, z.x, w.x) + float4(x.z, y.z, z.z, w.z));
}

// necessary but not sufficient junction condition for orthogonal edges
float clear(float2 crn, float2 a, float2 b)
{
    return (crn.x >= max(min(a.x, a.y), min(b.x, b.y))) && (crn.y >= max(min(a.x, b.y), min(b.x, a.y))) ? 1.0f : 0.0f;
}

float4 main(float2 texCoord : TEXCOORD0) : SV_Target0
{
    /*  grid        metric      pattern

        A B C       x y z       x y
        D E F         o w       w z
        G H I
    */

#define TEXm(x, y) Metric.SampleLevel(MetricSampler, texCoord, 0.0f, int2(x, y))
#define TEXs(x, y) Source.SampleLevel(SourceSampler, texCoord, 0.0f, int2(x, y))

    // metric data
    const float4 A = TEXm(-1, -1), B = TEXm( 0, -1);
    const float4 D = TEXm(-1,  0), E = TEXm( 0,  0), F = TEXm( 1,  0);
    const float4 G = TEXm(-1,  1), H = TEXm( 0,  1), I = TEXm( 1,  1);

    // strength data
    const float4 As = TEXs(-1, -1), Bs = TEXs( 0, -1), Cs = TEXs( 1, -1);
    const float4 Ds = TEXs(-1,  0), Es = TEXs( 0,  0), Fs = TEXs( 1,  0);
    const float4 Gs = TEXs(-1,  1), Hs = TEXs( 0,  1), Is = TEXs( 1,  1);

    // strength & dominance junctions
    const float4 jSx = float4(As.z, Bs.w, Es.x, Ds.y), jDx = dom(As.yzw, Bs.zwx, Es.wxy, Ds.xyz);
    const float4 jSy = float4(Bs.z, Cs.w, Fs.x, Es.y), jDy = dom(Bs.yzw, Cs.zwx, Fs.wxy, Es.xyz);
    const float4 jSz = float4(Es.z, Fs.w, Is.x, Hs.y), jDz = dom(Es.yzw, Fs.zwx, Is.wxy, Hs.xyz);
    const float4 jSw = float4(Ds.z, Es.w, Hs.x, Gs.y), jDw = dom(Ds.yzw, Es.zwx, Hs.wxy, Gs.xyz);

    // majority vote for ambiguous dominance junctions
    const float4 zero4 = 0.0f;
    const float4 jx = min(GE(jDx, zero4) * (LEQ(jDx.yzwx, zero4) * LEQ(jDx.wxyz, zero4) + GE(jDx + jDx.zwxy, jDx.yzwx + jDx.wxyz)), 1.0f);
    const float4 jy = min(GE(jDy, zero4) * (LEQ(jDy.yzwx, zero4) * LEQ(jDy.wxyz, zero4) + GE(jDy + jDy.zwxy, jDy.yzwx + jDy.wxyz)), 1.0f);
    const float4 jz = min(GE(jDz, zero4) * (LEQ(jDz.yzwx, zero4) * LEQ(jDz.wxyz, zero4) + GE(jDz + jDz.zwxy, jDz.yzwx + jDz.wxyz)), 1.0f);
    const float4 jw = min(GE(jDw, zero4) * (LEQ(jDw.yzwx, zero4) * LEQ(jDw.wxyz, zero4) + GE(jDw + jDw.zwxy, jDw.yzwx + jDw.wxyz)), 1.0f);

    // inject strength without creating new contradictions
    float4 res;
    res.x = min(jx.z + NOT(jx.y) * NOT(jx.w) * GE(jSx.z, 0.0f) * (jx.x + GE(jSx.x + jSx.z, jSx.y + jSx.w)), 1.0f);
    res.y = min(jy.w + NOT(jy.z) * NOT(jy.x) * GE(jSy.w, 0.0f) * (jy.y + GE(jSy.y + jSy.w, jSy.x + jSy.z)), 1.0f);
    res.z = min(jz.x + NOT(jz.w) * NOT(jz.y) * GE(jSz.x, 0.0f) * (jz.z + GE(jSz.x + jSz.z, jSz.y + jSz.w)), 1.0f);
    res.w = min(jw.y + NOT(jw.x) * NOT(jw.z) * GE(jSw.y, 0.0f) * (jw.w + GE(jSw.y + jSw.w, jSw.x + jSw.z)), 1.0f);

    // single pixel & end of line detection
    res = min(res * (float4(jx.z, jy.w, jz.x, jw.y) + NOT(res.wxyz * res.yzwx)), 1.0f);

    // output
    float4 clr;
    clr.x = clear(float2(D.z, E.x), float2(D.w, E.y), float2(A.w, D.y));
    clr.y = clear(float2(F.x, E.z), float2(E.w, E.y), float2(B.w, F.y));
    clr.z = clear(float2(H.z, I.x), float2(E.w, H.y), float2(H.w, I.y));
    clr.w = clear(float2(H.x, G.z), float2(D.w, H.y), float2(G.w, G.y));

    const float4 h = float4(min(D.w, A.w), min(E.w, B.w), min(E.w, H.w), min(D.w, G.w));
    const float4 v = float4(min(E.y, D.y), min(E.y, F.y), min(H.y, I.y), min(H.y, G.y));

    const float4 orien = GE(h + float4(D.w, E.w, E.w, D.w), v + float4(E.y, E.y, H.y, H.y));  // orientation
    const float4 hori = LE(h, v) * clr;  // horizontal edges
    const float4 vert = GE(h, v) * clr;  // vertical edges

    return (res + 2.0f * hori + 4.0f * vert + 8.0f * orien) / 15.0f;
}
