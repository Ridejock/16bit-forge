// SPDX-License-Identifier: MIT
// Hyllian's xBR-lv2 edge-smoothing scaler, ported to HLSL for SDL_GPU from libretro's
// slang-shaders, edge-smoothing/xbr/shaders/xbr-lv2-standalone.slang. Changes from the original:
// the per-vertex texel offsets are computed here in the fragment shader (all passes share
// fullscreen.vert.hlsl), the tunables are constants (the original defaults, AA blending on,
// corner C), and mat4x3 math is spelled with float4x3 rows. Each pass scales its input by the
// preset's factor (2x per pass); sample it with the "nearest" sampler.
//
// The original's license (MIT), kept as it requires:
/*
   Hyllian's xBR-lv2 Shader

   Copyright (C) 2011-2022 Hyllian - sergiogdb@gmail.com

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to permit
   persons to whom the Software is furnished to do so, subject to the
   following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.

   Incorporates some of the ideas from SABR shader. Thanks to Joshua Street.
*/
#include "filter_common.hlsli"

static const float kEqThreshold = 0.32f;    // color distinction threshold
static const float kLv2Coefficient = 0.30f; // smoothness threshold
static const float kLv2Cf = kLv2Coefficient + 2.0f;

static const float4 Ao = float4(1.0f, -1.0f, -1.0f, 1.0f);
static const float4 Bo = float4(1.0f, 1.0f, -1.0f, -1.0f);
static const float4 Co = float4(1.5f, 0.5f, -0.5f, 0.5f);
static const float4 Ax = float4(1.0f, -1.0f, -1.0f, 1.0f);
static const float4 Bx = float4(0.5f, 2.0f, -0.5f, -2.0f);
static const float4 Cx = float4(1.0f, 1.0f, -0.5f, 0.0f);
static const float4 Ay = float4(1.0f, -1.0f, -1.0f, 1.0f);
static const float4 By = float4(2.0f, 0.5f, -2.0f, -0.5f);
static const float4 Cy = float4(2.0f, 0.0f, -1.0f, 0.5f);
static const float4 Ci = float4(0.25f, 0.25f, 0.25f, 0.25f);

static const float3 V2F = float3(65536.0f, 256.0f, 1.0f); // color -> one float, for equality
static const float3 Y = float3(0.2627f, 0.6780f, 0.0593f);

float4 LTE(float4 a, float4 b) { return step(a, b); }
float4 LT(float4 a, float4 b) { return float4(a < b); }
float4 NOT(float4 a) { return 1.0f - a; }
float4 Diff(float4 a, float4 b) { return float4(a != b); }

float Dist(float3 a, float3 b) { return dot(abs(a - b), Y); }

// Color distance between four pixel pairs (row i of a with row i of b).
float4 Dist4(float4x3 a, float4x3 b)
{
    return float4(Dist(a[0], b[0]), Dist(a[1], b[1]), Dist(a[2], b[2]), Dist(a[3], b[3]));
}

float4 Eq(float4x3 a, float4x3 b) { return step(Dist4(a, b), kEqThreshold); }
float4 Neq(float4x3 a, float4x3 b) { return 1.0f - Eq(a, b); }

float4 WeightedDistance(float4x3 a, float4x3 b, float4x3 c, float4x3 d, float4x3 e, float4x3 f,
                        float4x3 g, float4x3 h)
{
    return Dist4(a, b) + Dist4(a, c) + Dist4(d, e) + Dist4(d, f) + 4.0f * Dist4(g, h);
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    const float2 texCoord = uv * 1.0001f;
    const float aaFactor = 2.0f * OutputSize.z * SourceSize.x;
    const float dx = SourceSize.z;
    const float dy = SourceSize.w;

    //    A1 B1 C1
    // A0  A  B  C C4
    // D0  D  E  F F4
    // G0  G  H  I I4
    //    G5 H5 I5
    const float4 t1 = texCoord.xxxy + float4(-dx, 0, dx, -2.0f * dy); // A1 B1 C1
    const float4 t2 = texCoord.xxxy + float4(-dx, 0, dx, -dy);        //  A  B  C
    const float4 t3 = texCoord.xxxy + float4(-dx, 0, dx, 0);          //  D  E  F
    const float4 t4 = texCoord.xxxy + float4(-dx, 0, dx, dy);         //  G  H  I
    const float4 t5 = texCoord.xxxy + float4(-dx, 0, dx, 2.0f * dy);  // G5 H5 I5
    const float4 t6 = texCoord.xyyy + float4(-2.0f * dx, -dy, 0, dy); // A0 D0 G0
    const float4 t7 = texCoord.xyyy + float4(2.0f * dx, -dy, 0, dy);  // C4 F4 I4

    const float2 fp = frac(texCoord * SourceSize.xy);

    const float3 A1 = SampleSource(t1.xw), B1 = SampleSource(t1.yw), C1 = SampleSource(t1.zw);
    const float3 A = SampleSource(t2.xw), B = SampleSource(t2.yw), C = SampleSource(t2.zw);
    const float3 D = SampleSource(t3.xw), E = SampleSource(t3.yw), F = SampleSource(t3.zw);
    const float3 G = SampleSource(t4.xw), H = SampleSource(t4.yw), I = SampleSource(t4.zw);
    const float3 G5 = SampleSource(t5.xw), H5 = SampleSource(t5.yw), I5 = SampleSource(t5.zw);
    const float3 A0 = SampleSource(t6.xy), D0 = SampleSource(t6.xz), G0 = SampleSource(t6.xw);
    const float3 C4 = SampleSource(t7.xy), F4 = SampleSource(t7.xz), I4 = SampleSource(t7.xw);

    const float4x3 b = float4x3(B, D, H, F);
    const float4x3 c = float4x3(C, A, G, I);
    const float4x3 d = float4x3(D, H, F, B);
    const float4x3 e = float4x3(E, E, E, E);
    const float4x3 f = float4x3(F, B, D, H);
    const float4x3 g = float4x3(G, I, C, A);
    const float4x3 h = float4x3(H, F, B, D);
    const float4x3 i = float4x3(I, C, A, G);

    const float4x3 i4 = float4x3(I4, C1, A0, G5);
    const float4x3 i5 = float4x3(I5, C4, A1, G0);
    const float4x3 h5 = float4x3(H5, F4, B1, D0);
    const float4x3 f4 = float4x3(F4, B1, D0, H5);

    const float4 b_ = mul(b, V2F);
    const float4 c_ = mul(c, V2F);
    const float4 d_ = b_.yzwx;
    const float4 e_ = mul(e, V2F);
    const float4 f_ = b_.wxyz;
    const float4 g_ = c_.zwxy;
    const float4 h_ = b_.zwxy;

    // These inequations define the line below which interpolation occurs.
    const float4 fx = Ao * fp.y + Bo * fp.x;
    const float4 fx_l = Ax * fp.y + Bx * fp.x;
    const float4 fx_u = Ay * fp.y + By * fp.x;

    const float4 irlv0 = Diff(e_, f_) * Diff(e_, h_);
    // Corner detection C.
    const float4 irlv1 = saturate(irlv0 * (Neq(f, b) * Neq(f, c) + Neq(h, d) * Neq(h, g) +
                                           Eq(e, i) * (Neq(f, f4) * Neq(f, i4) + Neq(h, h5) * Neq(h, i5)) +
                                           Eq(e, g) + Eq(e, c)));
    const float4 irlv2l = Diff(e_, g_) * Diff(d_, g_);
    const float4 irlv2u = Diff(e_, c_) * Diff(b_, c_);

    // Anti-aliased (blended) edges.
    const float4 delta = aaFactor;
    const float4 deltaL = float4(0.5f, 1.0f, 0.5f, 1.0f) * aaFactor;
    const float4 deltaU = deltaL.yxwz;
    float4 fx45i = saturate(0.5f + (fx - Co - Ci) / delta);
    float4 fx45 = saturate(0.5f + (fx - Co) / delta);
    float4 fx30 = saturate(0.5f + (fx_l - Cx) / deltaL);
    float4 fx60 = saturate(0.5f + (fx_u - Cy) / deltaU);

    const float4 wd1 = WeightedDistance(e, c, g, i, h5, f4, h, f);
    const float4 wd2 = WeightedDistance(h, d, i5, f, i4, b, e, i);

    const float4 d_fg = Dist4(f, g);
    const float4 d_hc = Dist4(h, c);

    const float4 edri = LTE(wd1, wd2) * irlv0;
    const float4 edr = LT(wd1, wd2) * irlv1 * NOT(edri.yzwx * edri.wxyz);
    const float4 edr_l = LTE(kLv2Cf * d_fg, d_hc) * irlv2l * edr * (NOT(edri.yzwx) * Eq(e, c));
    const float4 edr_u = LTE(kLv2Cf * d_hc, d_fg) * irlv2u * edr * (NOT(edri.wxyz) * Eq(e, g));

    fx45i = edri * fx45i;
    fx45 = edr * fx45;
    fx30 = edr_l * fx30;
    fx60 = edr_u * fx60;

    const float4 px = LTE(Dist4(e, f), Dist4(e, h));
    const float4 maximos = max(max(fx30, fx60), max(fx45, fx45i));

    float3 res1 = lerp(E, lerp(H, F, px.x), maximos.x);
    float3 res2 = lerp(E, lerp(B, D, px.z), maximos.z);
    const float3 res1a = lerp(res1, res2, step(Dist(E, res1), Dist(E, res2)));

    res1 = lerp(E, lerp(F, B, px.y), maximos.y);
    res2 = lerp(E, lerp(D, H, px.w), maximos.w);
    const float3 res1b = lerp(res1, res2, step(Dist(E, res1), Dist(E, res2)));

    const float3 res = lerp(res1a, res1b, step(Dist(E, res1a), Dist(E, res1b)));
    return float4(res, 1.0f);
}
