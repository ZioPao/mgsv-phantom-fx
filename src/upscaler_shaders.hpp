#pragma once

#include "hdr_shaders.hpp"

// HLSL for DLSS or FSR in place of the game's FXAA, compiled at runtime with d3dcompiler_47.
//
// Fox Engine tonemaps early and grades with a LUT, so the image FXAA reads is final LDR colour,
// stored sRGB-encoded. DLSS runs in LDR mode on those values and the composite writes them back
// where FXAA wrote, before the UI draws. With HDR (hdr.hpp) the image holds light beyond white: DLSS
// runs in its HDR mode on it, linear, and the composite encodes its result again. FSR always gets
// linear colour, which it is made for.
namespace phantom_fx::upscaler::shaders
{
// Runs once per frame at render resolution and writes what DLSS or FSR reads.
//
// Motion: the camera part is reprojected from depth with this and the previous frame's unjittered
// matrices, in float, so it is exact for everything that stands still. The engine also draws its
// dynamic meshes (characters, the horse, trees in the wind) into a velocity target, every frame
// whatever the game's motion blur setting (motion_blur.hpp): R = 1 where one drew, B and A the
// screen motion from the previous frame to this one in pixels / 64 (y down), stored as 0.5 v + 0.5
// in 8 bits. Each stored value covers a cell about 0.5 pixel wide, and the motion includes this
// frame's jitter, because the engine projects the current position with the jittered matrices and
// the previous one without. The object motion is the cell, less the jitter; the camera motion
// clamped into it keeps full precision where a character does not move.
inline constexpr char Inputs[] = PHANTOM_FX_HDR_COMMON R"(
cbuffer InputParameters : register(b0)
{
    float4 PreviousFromCurrent[4]; // Previous clip position from this frame's unjittered NDC and depth
    float2 JitterNdc;
    uint2 RenderSize;
    uint ObjectMotion; // t2 holds the engine's velocity
    uint Linear;       // The upscaler gets linear colour: FSR, or DLSS in its HDR mode
    uint2 Padding;
};
Texture2D<float4> SceneColor : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float4> Velocity : register(t2);
RWTexture2D<float4> OutColor : register(u0);
RWTexture2D<float> OutDepth : register(u1);
RWTexture2D<float2> OutMotion : register(u2);

static const float HalfCell = 64.0 / 255.0; // Half the engine's velocity step, in pixels

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= RenderSize)) return;

    float2 toPixels = float2(0.5, -0.5) * RenderSize;
    float2 ndc = (pixel + 0.5) / toPixels + float2(-1, 1) - JitterNdc;
    float depth = SceneDepth[pixel];
    float4 position = float4(ndc, depth, 1);
    float4 previous = float4(dot(PreviousFromCurrent[0], position), dot(PreviousFromCurrent[1], position),
        dot(PreviousFromCurrent[2], position), dot(PreviousFromCurrent[3], position));
    // Pixels from here to the previous position, which DLSS calls the motion vector.
    float2 motion = previous.w > 1e-6 ? (previous.xy / previous.w - ndc) * toPixels : 0;

    // The engine clamps its velocity to 64 pixels; beyond that only the camera motion is known.
    float4 velocity = Velocity[pixel];
    float2 stored = (velocity.ba * 2 - 1) * 64;
    if (ObjectMotion && velocity.r > 0.5 && length(stored) < 63)
    {
        float2 moved = stored - JitterNdc * toPixels;
        motion = clamp(motion, -moved - HalfCell, -moved + HalfCell);
    }

    const float4 color = SceneColor[pixel];
    OutColor[pixel] = Linear ? float4(min(SrgbToLinear(color.rgb), HalfMax), color.a) : color;
    OutDepth[pixel] = depth;
    OutMotion[pixel] = motion;
}
)";

// A triangle that covers the render target.
inline constexpr char FullScreen[] = R"(
float4 main(uint vertex : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

// Writes the upscaled output where FXAA wrote, sharpened with AMD FidelityFX RCAS (FsrRcasF in
// ffx_fsr1.h, MIT: AMD-FidelityFX-LICENSE.txt), whose noise filter is left out. RCAS expects
// perceptual values in 0 to 1, which the sRGB-encoded LDR colour is. FXAA writes alpha 0.
inline constexpr char Composite[] = PHANTOM_FX_HDR_COMMON R"(
cbuffer CompositeParameters : register(b0)
{
    uint DebugView; // 1 motion vectors, 2 where the engine's velocity was used
    float Sharpness;
    uint ObjectMotion;
    uint Linear; // The upscaler made linear colour, sharpened through x / (1 + x) and encoded here
};
Texture2D<float4> Upscaled : register(t0);
Texture2D<float2> Motion : register(t1);
Texture2D<float4> Velocity : register(t2);

float3 Load(int2 pixel)
{
    uint width, height;
    Upscaled.GetDimensions(width, height);
    const float3 color = Upscaled.Load(int3(clamp(pixel, 0, int2(width, height) - 1), 0)).rgb;
    // RCAS takes values in 0 to 1; light maps there through x / (1 + x), and back after.
    return Linear ? max(color, 0) / (1 + max(color, 0)) : saturate(color);
}

// b, d, f and h are the pixels above, left, right and below e.
float3 Rcas(float3 b, float3 d, float3 e, float3 f, float3 h)
{
    float3 low = min(min(b, d), min(f, h)), high = max(max(b, d), max(f, h));
    // The lobe at which each channel would clip below 0 or above 1.
    float3 hitMin = min(low, e) / max(4 * high, 1.0 / 65536);
    float3 hitMax = (1 - max(high, e)) / min(4 * low - 4, -1.0 / 65536);
    float3 lobes = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16), min(max(lobes.r, max(lobes.g, lobes.b)), 0)) * Sharpness;
    return (lobe * (b + d + f + h) + e) / (4 * lobe + 1);
}

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float3 color = Load(pixel);
    if (DebugView == 1)
    {
        float2 motion = Motion.Load(int3(pixel, 0));
        color = float3(saturate(0.5 + motion * 0.05), saturate(length(motion) * 0.05));
    }
    else if (DebugView == 2)
    {
        bool used = ObjectMotion && Velocity.Load(int3(pixel, 0)).r > 0.5;
        color = lerp(color * 0.5, float3(1, 0, 0), used ? 0.6 : 0);
    }
    else if (Sharpness > 0)
        color = Rcas(Load(pixel + int2(0, -1)), Load(pixel + int2(-1, 0)), color, Load(pixel + int2(1, 0)), Load(pixel + int2(0, 1)));

    if (Linear && DebugView == 0) color = LinearToSrgb(color / max(1 - color, 1.0 / HalfMax));

    return float4(color, 0);
}
)";

// Stretches the engine's render-size scene over the UI target when DLSS did not run, as
// GammaControl would have: bilinear, in the same sRGB-encoded values.
inline constexpr char StretchVertex[] = R"(
void main(uint vertex : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((vertex << 1) & 2, vertex & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

inline constexpr char Stretch[] = R"(
Texture2D<float4> Scene : register(t0);
SamplerState Linear : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return Scene.SampleLevel(Linear, uv, 0);
}
)";
}
