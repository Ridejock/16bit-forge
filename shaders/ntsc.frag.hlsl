// SPDX-License-Identifier: MIT
// NTSC video signal: encodes each emulated line the way the SNES does (YIQ, chroma modulated
// onto a 3.58 MHz subcarrier) and decodes it again like a simple TV, so the picture gets the
// colour bleed, rainbow fringes on fine detail and dot crawl of a composite cable, or the
// cleaner S-Video look (luma and chroma on separate wires, so no cross-talk).
//
// Original code (MIT, Ervin Ashley), written from the published NTSC signal math and the
// SNES's video timing, not derived from any existing filter:
//   - Timing: the subcarrier is the 21.477 MHz master clock / 6. A low-res pixel lasts 4 master
//     clocks (hi-res: 2), so the carrier advances 2/3 of a cycle per pixel; a line lasts 1364
//     clocks, so each line starts 1/3 cycle later; and every other frame has a 4-clock-shorter
//     line, so frames alternate by 1/3 cycle (the source of the dot crawl's 30 Hz shimmer).
//   - Encode: YIQ with the FCC 1953 matrix; composite = Y + I cos(phase) + Q sin(phase).
//   - Decode: chroma by product demodulation (multiply by cos / sin, then a triangle low-pass one
//     carrier cycle wide per side, which nulls the carrier harmonics and luma DC exactly). The
//     composite luma is the signal minus the re-modulated chroma estimate, so luma detail that
//     overlaps the carrier leaks into colour (rainbows) and colour edges leak into luma (dots).
//
// Preset: the first pass, "source, 4x1, nearest": output has 4 signal samples per emulated pixel
// (one per master clock in low-res) on each line, and goes to a CRT pass such as crt_lottes.
#include "filter_common.hlsli"

// @param ntsc_svideo 0.0         0 = composite, 1 = S-Video (separate luma and chroma)
// @param ntsc_temporal 1.0       1 = alternate the carrier phase every frame like the console
// @param ntsc_chroma_width 6.0   chroma low-pass half-width in master clocks (keep a multiple of 6)
// @param ntsc_saturation 1.0     colour saturation of the decoded picture
#define kSVideo Parameter(0)
#define kTemporal Parameter(1)
#define kChromaWidth Parameter(2)
#define kSaturation Parameter(3)

static const float kPi2 = 6.28318531f;
static const float kMasterClocksPerLine = 1024.0f;  // 256 low-res pixels x 4 clocks (visible part)
static const float kClocksPerCycle = 6.0f;           // master clock / subcarrier
static const int kMaxTaps = 16;                      // per side; covers 12 clocks in hi-res too

static const float3x3 kRgbToYiq = float3x3(0.299f, 0.587f, 0.114f,
                                           0.5959f, -0.2746f, -0.3213f,
                                           0.2115f, -0.5227f, 0.3112f);
static const float3x3 kYiqToRgb = float3x3(1.0f, 0.956f, 0.619f,
                                           1.0f, -0.272f, -0.647f,
                                           1.0f, -1.106f, 1.703f);

// The signal at sample index `sample` of the current line: x = luma (composite: the whole
// signal), y = chroma (S-Video only, else 0), z = subcarrier phase in radians.
float3 Signal(float sample, float row, float clocksPerSample, float cycle0)
{
    const float samplesPerPixel = OutputSize.x * SourceSize.z;
    const float pixel = floor((sample + 0.5f) / samplesPerPixel);
    const float2 uv = float2((pixel + 0.5f) * SourceSize.z, (row + 0.5f) * SourceSize.w);
    const float3 yiq = mul(kRgbToYiq, Source.SampleLevel(SourceSampler, uv, 0.0f).rgb);
    const float phase = kPi2 * frac(cycle0 + sample * clocksPerSample / kClocksPerCycle);
    const float chroma = yiq.y * cos(phase) + yiq.z * sin(phase);
    return kSVideo > 0.5f ? float3(yiq.x, chroma, phase) : float3(yiq.x + chroma, 0.0f, phase);
}

float4 main(float2 texCoord : TEXCOORD0) : SV_Target0
{
    const float sample = floor(texCoord.x * OutputSize.x);
    const float row = floor(texCoord.y * SourceSize.y);
    const float clocksPerSample = kMasterClocksPerLine * OutputSize.z;
    const float frame = kTemporal > 0.5f ? float(FrameCount & 1u) : 0.0f;
    const float cycle0 = (row + frame) / 3.0f;  // carrier cycles at the line start

    // Demodulate: low-passed signal x 2 cos / 2 sin around this sample.
    float2 iq = 0.0f;
    float total = 0.0f;
    for (int k = -kMaxTaps; k <= kMaxTaps; k++) {
        const float w = max(1.0f - abs(float(k)) * clocksPerSample / kChromaWidth, 0.0f);
        const float3 s = Signal(sample + float(k), row, clocksPerSample, cycle0);
        const float c = kSVideo > 0.5f ? s.y : s.x;
        iq += w * c * float2(cos(s.z), sin(s.z));
        total += w;
    }
    iq *= 2.0f / total;

    const float3 here = Signal(sample, row, clocksPerSample, cycle0);
    const float luma = kSVideo > 0.5f ? here.x : here.x - (iq.x * cos(here.z) + iq.y * sin(here.z));
    const float3 rgb = mul(kYiqToRgb, float3(luma, iq * kSaturation));
    return float4(saturate(rgb), 1.0f);
}
