// SPDX-License-Identifier: MIT
// CRT look after Timothy Lottes' public-domain "CRT styled scan-line shader": each emulated
// pixel is a soft Gaussian spot, lines are Gaussian beams (the nearest three lines blend), a
// small bloom adds glow, and a shadow mask / aperture grille tints the output pixels. Ported to
// HLSL for SDL_GPU from libretro slang-shaders crt/shaders/crt-lottes.slang (which keeps the
// original public-domain notice below). Changes from the original: the tunables are @param
// parameters (same defaults; gamma-correct scaling always on); its warpX/warpY curvature is
// replaced by our own barrel distortion and rounded, soft-edged screen corners (original MIT
// code, Curve() and ScreenMask() below), flat by default. Meant for the last pass at viewport
// size; it fetches its input texels itself, so either sampler works.
//
// Original notice:
//   PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER
//     by Timothy Lottes
//   This is more along the style of a really good CGA arcade monitor. With RGB inputs instead
//   of NTSC. The shadow mask example has the mask rotated 90 degrees for less chromatic
//   aberration. Left it unoptimized to show the theory behind the algorithm. It is an example
//   what I personally would want as a display option for pixel art games. Please take and use,
//   change, or whatever.
#include "filter_common.hlsli"

// @param hard_scan -8.0          scanline beam sharpness (more negative = sharper)
// @param hard_pix -3.0           horizontal pixel sharpness
// @param mask_dark 0.5           mask: dimmed sub-pixels
// @param mask_light 1.5          mask: lit sub-pixel
// @param shadow_mask 3.0         0 off, 1 compressed TV, 2 aperture grille, 3 stretched VGA, 4 VGA
// @param bright_boost 1.0        brightness multiplier
// @param hard_bloom_pix -1.5     bloom: horizontal softness
// @param hard_bloom_scan -2.0    bloom: vertical softness
// @param bloom_amount 0.15       bloom strength
// @param shape 2.0               kernel shape exponent (2 = Gaussian)
// @param curvature 0.0           barrel distortion k (0 = flat screen)
// @param corner_radius 0.0       rounded screen corners, as a fraction of the half-height
// @param corner_softness 0.0     width of the fade at the screen edge, in half-heights (0 = hard edge)
#define kHardScan Parameter(0)
#define kHardPix Parameter(1)
#define kMaskDark Parameter(2)
#define kMaskLight Parameter(3)
#define kShadowMask Parameter(4)
#define kBrightBoost Parameter(5)
#define kHardBloomPix Parameter(6)
#define kHardBloomScan Parameter(7)
#define kBloomAmount Parameter(8)
#define kShape Parameter(9)
#define kCurvature Parameter(10)
#define kCornerRadius Parameter(11)
#define kCornerSoftness Parameter(12)

float ToLinear1(float c) { return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f); }
float3 ToLinear(float3 c) { return float3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b)); }
float ToSrgb1(float c) { return c < 0.0031308f ? c * 12.92f : 1.055f * pow(c, 0.41666f) - 0.055f; }
float3 ToSrgb(float3 c) { return float3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b)); }

// Nearest emulated sample given a position and a texel offset.
float3 Fetch(float2 pos, float2 off)
{
    pos = (floor(pos * SourceSize.xy + off) + 0.5f) * SourceSize.zw;
    return ToLinear(kBrightBoost * Source.SampleLevel(SourceSampler, pos, 0.0f).rgb);
}

// Distance in emulated pixels to the nearest texel center.
float2 Dist(float2 pos)
{
    pos *= SourceSize.xy;
    return -((pos - floor(pos)) - 0.5f);
}

// 1D Gaussian-like falloff.
float Gaus(float pos, float scale)
{
    return exp2(scale * pow(abs(pos), kShape));
}

// N-tap horizontal filter along one line, taps -R..R around the pixel.
float3 Horz(float2 pos, float off, int radius, float scale)
{
    const float dst = Dist(pos).x;
    float3 sum = 0.0f;
    float total = 0.0f;
    for (int i = -radius; i <= radius; i++) {
        const float w = Gaus(dst + float(i), scale);
        sum += Fetch(pos, float2(float(i), off)) * w;
        total += w;
    }
    return sum / total;
}

float Scan(float2 pos, float off, float scale)
{
    return Gaus(Dist(pos).y + off, scale);
}

// The nearest three lines (3, 5, 3 taps).
float3 Tri(float2 pos)
{
    return Horz(pos, -1.0f, 1, kHardPix) * Scan(pos, -1.0f, kHardScan) +
           Horz(pos, 0.0f, 2, kHardPix) * Scan(pos, 0.0f, kHardScan) +
           Horz(pos, 1.0f, 1, kHardPix) * Scan(pos, 1.0f, kHardScan);
}

// Small bloom over five lines (5, 7, 7, 7, 5 taps).
float3 Bloom(float2 pos)
{
    return Horz(pos, -2.0f, 2, kHardBloomPix) * Scan(pos, -2.0f, kHardBloomScan) +
           Horz(pos, -1.0f, 3, kHardBloomPix) * Scan(pos, -1.0f, kHardBloomScan) +
           Horz(pos, 0.0f, 3, kHardBloomPix) * Scan(pos, 0.0f, kHardBloomScan) +
           Horz(pos, 1.0f, 3, kHardBloomPix) * Scan(pos, 1.0f, kHardBloomScan) +
           Horz(pos, 2.0f, 2, kHardBloomPix) * Scan(pos, 2.0f, kHardBloomScan);
}

// Lights one of the three sub-pixels of a mask triad, by position 0..1 in the triad.
float3 Triad(float x)
{
    float3 mask = kMaskDark;
    if (x < 0.333f) mask.r = kMaskLight;
    else if (x < 0.666f) mask.g = kMaskLight;
    else mask.b = kMaskLight;
    return mask;
}

// Shadow mask, by output pixel position.
float3 Mask(float2 pos)
{
    if (kShadowMask == 1.0f) {  // very compressed TV style shadow mask
        const float odd = frac(pos.x / 6.0f) < 0.5f ? 1.0f : 0.0f;
        const float row = frac((pos.y + odd) * 0.5f) < 0.5f ? kMaskDark : kMaskLight;
        return Triad(frac(pos.x / 3.0f)) * row;
    }
    if (kShadowMask == 2.0f) return Triad(frac(pos.x / 3.0f));  // aperture grille
    if (kShadowMask == 3.0f) return Triad(frac((pos.x + pos.y * 3.0f) / 6.0f));  // stretched VGA
    pos = floor(pos * float2(1.0f, 0.5f));  // 4: VGA style shadow mask
    return Triad(frac((pos.x + pos.y * 3.0f) / 6.0f));
}

// Barrel distortion: uv in -1..1, pushed outwards by k * r^2 (k = 0: unchanged).
float2 Curve(float2 uv)
{
    uv = uv * 2.0f - 1.0f;
    uv *= 1.0f + kCurvature * dot(uv, uv);
    return uv * 0.5f + 0.5f;
}

// 1 inside the (curved) screen, fading to 0 across its edge; corners rounded by kCornerRadius.
float ScreenMask(float2 uv)
{
    const float2 p = abs(uv * 2.0f - 1.0f);  // 0 at the center, 1 at the edges
    const float2 q = p - (1.0f - kCornerRadius);
    const float outside = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - kCornerRadius;  // rounded-box distance
    return 1.0f - smoothstep(-max(kCornerSoftness, 1e-5f), 0.0f, outside);
}

float4 main(float2 texCoord : TEXCOORD0, float4 position : SV_Position) : SV_Target0
{
    const float2 pos = Curve(texCoord);
    float3 color = Tri(pos) + Bloom(pos) * kBloomAmount;
    if (kShadowMask > 0.0f) color *= Mask(texCoord * OutputSize.xy);
    color *= ScreenMask(pos);
    return float4(ToSrgb(saturate(color)), 1.0f);
}
