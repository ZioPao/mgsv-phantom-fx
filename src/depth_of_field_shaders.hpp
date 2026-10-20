#pragma once

// HLSL for the depth of field (depth_of_field.hpp), compiled at runtime with d3dcompiler_47. The
// gather follows the scatter-as-gather of Call of Duty: Advanced Warfare (Jorge Jimenez, SIGGRAPH
// 2014) and Unreal Engine's Diaphragm DOF (Guillaume Abadie, SIGGRAPH 2018): each pixel collects the
// samples whose blur reaches it, those in front of the focus apart from those behind it.
namespace phantom_fx::depth_of_field::shaders
{
// What every shader here shares: the constant buffer, the game's colour encodings and the blur
// radius (circle of confusion) of a depth.
#define PHANTOM_FX_COMMON R"(
cbuffer Parameters : register(b0)
{
    uint2 FullSize;      // Render size: the scene's and the depth's
    uint2 HalfSize;      // The gather's size, half the render size rounded up
    float2 HalfTexel;    // 1 / HalfSize
    float MaxRadius;     // The largest blur radius, render pixels
    uint Blades;         // Aperture blades; 0 is round
    uint UseLens;        // 1: photo mode's lens below; 0: the game's camera, from its own constants
    float Aperture;      // Photo mode: the aperture's diameter, metres
    float FocusDistance; // Photo mode: metres
    float GameUnit;      // Render pixels per unit of the game's blur radius
    float SampleScale;   // Lens shot: 1 / the frames added up
    uint LightInScene;   // HDR: the scene holds light, which needs no undoing of the tonemap
    float2 Padding;
};

static const float Pi = 3.14159265;
static const float GoldenAngle = 2.39996323;
// The aperture's corners point this way, as photo_mode.cpp's lens points do.
static const float BladeRotation = Pi / 2;

// TppTonemap, the deferred resolve's tonemap: x up to b, then b + c - 1 / (a (x - b + c)), which
// approaches b + c. Tonemap[0] holds the resolve's (a, b, c), captured each frame; zeros, before
// the first capture, leave colours as they are. Blurring before the tonemap, as a lens does, keeps
// bright lights bright in the blur. With HDR the scene already holds light (LightInScene).
StructuredBuffer<float4> Tonemap : register(t5);
static const float MaxHdr = 64;

float3 ToHdr(float3 y)
{
    const float a = Tonemap[0].x, b = Tonemap[0].y, c = Tonemap[0].z;
    if (LightInScene || !(a > 0 && c > 0)) return y;

    // 8 bits round the brightest values to the curve's end; MaxHdr keeps them finite.
    const float3 headroom = max(b + c - y, 1 / (a * (MaxHdr - b + c)));
    return y <= b ? y : b - c + 1 / (a * headroom);
}

float3 FromHdr(float3 x)
{
    const float a = Tonemap[0].x, b = Tonemap[0].y, c = Tonemap[0].z;
    if (LightInScene || !(a > 0 && c > 0)) return x;

    return x <= b ? x : b + c - 1 / (a * (max(x, b) - b + c));
}

// The scene target holds sRGB, read and written here through UNORM views.
float3 SrgbToLinear(float3 c)
{
    c = max(c, 0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// Light beyond 1 only comes with HDR, whose float target keeps it; the 8-bit one clamps.
float3 LinearToSrgb(float3 c)
{
    c = max(c, 0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055;
}

// The blur radius of a reversed-Z depth, in render pixels: negative in front of the focus. `lens`
// is (scale, focus distance, nearest limit, farthest limit), `depth` the projection's (B, A) for
// depth = A + B / z, as the game's ReconstructViewZ takes them.
float BlurOf(float deviceDepth, float4 lens, float2 depth)
{
    const float z = depth.x / (deviceDepth - depth.y); // Infinite on the sky
    return clamp(lens.x * (1 - lens.y / z), -lens.z, lens.w);
}

// How far the aperture's edge is from its centre in the direction of `offset`: 1 for a round
// aperture, otherwise a regular polygon with its corners at 1.
float ApertureEdge(float2 offset)
{
    if (Blades == 0) return 1;

    const float sector = 2 * Pi / Blades;
    const float angle = atan2(offset.y, offset.x) - BladeRotation;
    const float local = angle - sector * floor(angle / sector) - sector / 2;
    return cos(sector / 2) / cos(local);
}

// How much of a sample blurred over `radius` lands `distance` away in the direction of `offset`.
float Coverage(float radius, float2 offset, float distance, float softness)
{
    return saturate((radius * ApertureEdge(offset) - distance) / softness + 0.5);
}

// A Vogel spiral: `count` points spread evenly over the unit disc.
float2 DiscPoint(uint index, uint count)
{
    const float angle = index * GoldenAngle;
    return sqrt((index + 0.5) / count) * float2(cos(angle), sin(angle));
}
)"

// Half size: the scene through the inverse tonemap and the blur radius of each pixel, the largest
// blur of each 8x8 tile, and the lens the later passes use. Each half-size pixel takes the blur
// furthest from sharp among its four, preferring the foreground, and the colours whose blur is
// close to it, so a sharp edge does not seep into the blur next to it.
inline constexpr char Setup[] = PHANTOM_FX_COMMON R"(
// DOF_ScatterBakeFirst's constants: cPSObject c11 = (near/far depth, HDR limit, farthest blur, nearest
// blur), c12 = (smallest, largest, scale, focus distance); cPSScene c4 = the projection's first row,
// c21 = (1 / P00, 1 / P11, B, A).
cbuffer GameObject : register(b1)
{
    float4 GameObject[13];
};

cbuffer GameScene : register(b2)
{
    float4 GameScene[30];
};

Texture2D<float4> Scene : register(t0);
Texture2D<float> Depth : register(t1);
RWTexture2D<float4> HalfColor : register(u0);
RWTexture2D<float> TileBlur : register(u1);
RWStructuredBuffer<float4> OutLens : register(u2);

groupshared uint tileMaximum;

float4 LensParameters()
{
    // A thin lens seen through the projection: a point at z is spread over the aperture's diameter
    // times |1 / focus - 1 / z| in NDC per unit of P00.
    if (UseLens) return float4(0.25 * FullSize.x * abs(GameScene[4].x) * Aperture / FocusDistance, FocusDistance, MaxRadius, MaxRadius);

    return float4(GameObject[12].z * GameUnit, GameObject[12].w, min(GameObject[11].w * GameUnit, MaxRadius),
        min(GameObject[11].z * GameUnit, MaxRadius));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID, uint index : SV_GroupIndex, uint2 tile : SV_GroupID)
{
    if (index == 0) tileMaximum = 0;
    GroupMemoryBarrierWithGroupSync();

    const float4 lens = LensParameters();
    const float2 depth = GameScene[21].zw;
    if (all(id == 0))
    {
        OutLens[0] = lens;
        OutLens[1] = float4(depth, 0, 0);
    }

    float largest = 0;
    if (all(id < HalfSize))
    {
        float3 colours[4];
        float blurs[4];
        uint pick = 0;
        float best = -1;
        for (uint i = 0; i < 4; ++i)
        {
            const uint2 p = min(id * 2 + uint2(i & 1, i >> 1), FullSize - 1);
            colours[i] = ToHdr(SrgbToLinear(Scene.Load(int3(p, 0)).rgb));
            blurs[i] = BlurOf(Depth.Load(int3(p, 0)), lens, depth);
            const float score = abs(blurs[i]) + (blurs[i] < 0 ? 0.5 : 0);
            if (score > best)
            {
                best = score;
                pick = i;
            }
        }

        const float blur = blurs[pick];
        float3 sum = 0;
        float weights = 0;
        for (uint j = 0; j < 4; ++j)
        {
            const float weight = 1 / (1 + abs(blurs[j] - blur));
            sum += colours[j] * weight;
            weights += weight;
        }

        HalfColor[id] = float4(sum / weights, blur / 2);
        largest = abs(blur) / 2;
    }

    InterlockedMax(tileMaximum, asuint(largest));
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) TileBlur[tile] = asfloat(tileMaximum);
}
)";

// Per tile: the largest blur, in half-size pixels, that reaches it from the tiles around.
inline constexpr char Dilate[] = PHANTOM_FX_COMMON R"(
Texture2D<float> TileBlur : register(t0);
RWTexture2D<float> TileRadius : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_DispatchThreadID)
{
    const int2 tiles = int2((HalfSize + 7) / 8);
    if (any(int2(tile) >= tiles)) return;

    const int reach = (int)ceil(MaxRadius / 16);
    float radius = 0;
    for (int y = -reach; y <= reach; ++y)
        for (int x = -reach; x <= reach; ++x)
        {
            const int2 other = int2(tile) + int2(x, y);
            if (any(other < 0) || any(other >= tiles)) continue;

            // The tiles' nearest pixels are this far apart.
            const float gap = length(max(abs(float2(x, y)) - 1, 0)) * 8;
            const float blur = TileBlur[other];
            if (blur > gap) radius = max(radius, blur);
        }

    TileRadius[tile] = radius;
}
)";

// Half size: each pixel collects the samples whose blur covers it within its tile's radius. Every
// sample stands for the same share of that disc and spreads its light over its own blur's area,
// which gives each its weight. Samples in front of the focus cover whatever lies behind them; they
// add up to a foreground layer whose coverage is how much of the pixel they hide. Samples behind
// the focus that lie behind the centre reach it only as far as the centre's own blur: the centre
// hides the rest.
inline constexpr char Gather[] = PHANTOM_FX_COMMON R"(
Texture2D<float4> HalfColor : register(t0); // With mips, for wide blurs whose samples lie far apart
Texture2D<float> TileRadius : register(t1);
RWTexture2D<float4> Background : register(u0);
RWTexture2D<float4> Foreground : register(u1); // Premultiplied by its coverage
SamplerState Linear : register(s0);

static const uint MaxSamples = 64;

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID, uint2 tile : SV_GroupID)
{
    if (any(id >= HalfSize)) return;

    const float4 centre = HalfColor.Load(int3(id, 0));
    const float radius = TileRadius[tile];
    if (radius < 0.5)
    {
        Background[id] = float4(centre.rgb, 0);
        Foreground[id] = 0;
        return;
    }

    const uint count = clamp((uint)ceil(radius * radius * Pi / 2), 8, MaxSamples);
    const float spacing = radius * sqrt(Pi / count);
    const float lod = clamp(log2(spacing / 1.5), 0, 3);
    const float softness = max(1, spacing / 2);
    const float2 uv = (id + 0.5) * HalfTexel;

    float3 background = 0, foreground = 0;
    float backgroundWeight = 0, foregroundWeight = 0;
    const float centreWeight = 1 / max(centre.a * centre.a, 1);
    if (centre.a < 0)
    {
        foreground = centre.rgb * centreWeight;
        foregroundWeight = centreWeight;
    }
    else
    {
        background = centre.rgb * centreWeight;
        backgroundWeight = centreWeight;
    }

    for (uint i = 0; i < count; ++i)
    {
        const float2 offset = radius * DiscPoint(i, count);
        const float distance = length(offset);
        const float4 tap = HalfColor.SampleLevel(Linear, uv + offset * HalfTexel, lod);
        float weight = 1 / max(tap.a * tap.a, 1);
        if (tap.a < 0)
        {
            // A blur in front of the focus is the aperture's image turned over.
            weight *= Coverage(-tap.a, offset, distance, softness);
            foreground += tap.rgb * weight;
            foregroundWeight += weight;
        }
        else
        {
            const float reach = tap.a > centre.a ? min(tap.a, max(centre.a, 0)) : tap.a;
            weight *= Coverage(reach, -offset, distance, softness);
            background += tap.rgb * weight;
            backgroundWeight += weight;
        }
    }

    // A sample's weight is 1 / r^2; it lands on pi r^2 pixels and stands for pi R^2 / samples.
    const float coverage = saturate(foregroundWeight * radius * radius / (count + 1));
    Background[id] = float4(backgroundWeight > 1e-6 ? background / backgroundWeight : centre.rgb, 0);
    Foreground[id] = foregroundWeight > 0 ? float4(foreground / foregroundWeight * coverage, coverage) : 0;
}
)";

// Full size, in place of DOF_ScatterCompositeFinal: sharp pixels stay exactly as they were, blurs of
// a few pixels are gathered here at full size, wider ones come from the half-size gather, and the
// foreground goes over both. Back through the tonemap into the scene target, keeping its alpha.
inline constexpr char Composite[] = PHANTOM_FX_COMMON R"(
Texture2D<float4> Scene : register(t0); // The game's copy of the scene target
Texture2D<float> Depth : register(t1);
Texture2D<float4> Background : register(t2);
Texture2D<float4> Foreground : register(t3);
Texture2D<float4> HalfColor : register(t4);
StructuredBuffer<float4> Lens : register(t6);
SamplerState Linear : register(s0);

// Render pixels: blurs up to this one are gathered at full size, wider ones blend into the half-size gather.
static const float SlightLimit = 3;
static const uint SlightSamples = 12;

float BlurAt(int2 p)
{
    return BlurOf(Depth.Load(int3(p, 0)), Lens[0], Lens[1].xy);
}

float3 SlightBlur(int2 p, float blur, float3 centre)
{
    float3 sum = centre / max(blur * blur, 1);
    float total = 1 / max(blur * blur, 1);
    for (uint i = 0; i < SlightSamples; ++i)
    {
        const int2 offset = int2(round(abs(blur) * DiscPoint(i, SlightSamples)));
        const int2 q = clamp(p + offset, 0, int2(FullSize) - 1);
        const float sampleBlur = BlurAt(q);
        const float reach = sampleBlur > blur ? min(abs(sampleBlur), abs(blur)) : abs(sampleBlur);
        const float weight = Coverage(reach, sampleBlur < 0 ? offset : -offset, length(offset), 1) / max(sampleBlur * sampleBlur, 1);
        sum += ToHdr(SrgbToLinear(Scene.Load(int3(q, 0)).rgb)) * weight;
        total += weight;
    }

    return sum / total;
}

// The half-size background, from the four half-size pixels around weighted by how close their blur is.
float3 WideBlur(float2 position, float blur)
{
    const float2 halfPosition = position / 2 - 0.5;
    const int2 base = int2(floor(halfPosition));
    const float2 f = halfPosition - base;
    float3 sum = 0;
    float total = 0;
    for (uint i = 0; i < 4; ++i)
    {
        const int2 corner = int2(i & 1, i >> 1);
        const int2 q = clamp(base + corner, 0, int2(HalfSize) - 1);
        const float2 bilinear = corner ? f : 1 - f;
        const float weight = bilinear.x * bilinear.y / (1 + abs(HalfColor.Load(int3(q, 0)).a * 2 - blur));
        sum += Background.Load(int3(q, 0)).rgb * weight;
        total += weight;
    }

    return sum / max(total, 1e-6);
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 p = int2(position.xy);
    const float4 scene = Scene.Load(int3(p, 0));
    const float blur = BlurAt(p);
    const float4 foreground = Foreground.SampleLevel(Linear, position.xy / 2 * HalfTexel, 0);
    if (abs(blur) < 0.5 && foreground.a < 1.0 / 512) return scene;

    float3 colour = ToHdr(SrgbToLinear(scene.rgb));
    if (abs(blur) >= 0.5)
    {
        const float wide = saturate(abs(blur) - SlightLimit);
        const float3 slight = wide < 1 ? SlightBlur(p, blur, colour) : 0;
        colour = wide > 0 ? lerp(slight, WideBlur(position.xy, blur), wide) : slight;
    }

    colour = colour * (1 - foreground.a) + foreground.rgb;
    return float4(LinearToSrgb(FromHdr(colour)), scene.a);
}
)";

// Lens shot: the frame, through the inverse tonemap, added to the running sum.
inline constexpr char Accumulate[] = PHANTOM_FX_COMMON R"(
Texture2D<float4> Scene : register(t0);
RWStructuredBuffer<float3> Sum : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= FullSize)) return;

    const uint index = id.y * FullSize.x + id.x;
    Sum[index] += ToHdr(SrgbToLinear(Scene.Load(int3(id, 0)).rgb));
}
)";

// Lens shot, in place of DOF_ScatterCompositeFinal: the average so far, back through the tonemap.
inline constexpr char ShowShot[] = PHANTOM_FX_COMMON R"(
Texture2D<float4> Scene : register(t0); // For its alpha
StructuredBuffer<float3> Sum : register(t7);

float4 main(float4 position : SV_Position) : SV_Target
{
    const uint2 p = uint2(position.xy);
    const float3 average = Sum[p.y * FullSize.x + p.x] * SampleScale;
    return float4(LinearToSrgb(FromHdr(average)), Scene.Load(int3(p, 0)).a);
}
)";

// At the deferred resolve: its cPSMaterial c0, TppTonemap's (a, b, c), for the passes above.
inline constexpr char CaptureTonemap[] = R"(
cbuffer GameMaterial : register(b1)
{
    float4 GameMaterial[1];
};

RWStructuredBuffer<float4> OutTonemap : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    OutTonemap[0] = GameMaterial[0];
}
)";

#undef PHANTOM_FX_COMMON
}
