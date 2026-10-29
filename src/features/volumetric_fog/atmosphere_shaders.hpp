#pragma once

// HLSL for the volumetric fog and the physical sky (atmosphere.hpp), compiled at runtime with
// d3dcompiler_47. The physical atmosphere is ported from Sébastien Hillaire's reference for "A
// Scalable and Production Ready Sky and Atmosphere Rendering Technique" (Epic Games, 2020, MIT; its
// license is in licenses/).
namespace phantom_fx::atmosphere::shaders
{
// What every fog pass reads: the add-on's constants, copies of the constant buffers the game's fog
// pass (VolFog_TppVolFog) and sun pass bound, so the fog follows the game's weather and time of day,
// and the froxels' places.
#define PHANTOM_FX_FOG_INPUTS R"(
cbuffer Fog : register(b0)
{
    uint2 TileSize;      // Texels of one depth slice in the game's fog texture
    uint Shadowed;       // 1 when the sun pass's shadow map and constants are this frame's
    uint ShadowSamples;  // Shadow comparisons per froxel
    float2 Jitter;       // This frame's place in each froxel, across the view, in froxels from -0.5 to 0.5
    float DepthJitter;   // And through its slice, from 0 to 1
    float HistoryWeight; // Of the last frames' visibility, reprojected; 0 without
    uint Physical;       // 1: the physical look's air and height fog in place of the game's formula
    uint SkySun;         // 1: SkyMaterial holds a sky draw's constants
    float Haze;          // The physical look's haze, 0 to 1: how much of the game's fog its height fog takes
    float Padding;
};

cbuffer FogScene : register(b1) // cPSScene of the game's fog pass
{
    float4x4 FogProjectionView;
    float4x4 FogProjection;
    float4 FogPlacement[4];      // m_view, by which the fog pass places its froxels
    float4x4 FogShadowProjection;
    float4x4 FogShadowProjection2;
    float4 FogEyePosition;
    float4 FogProjectionParam;   // 1 / P00, 1 / P11, B, A
    float4 FogViewportSize;
    float4 FogExposure;
    float4 FogParam[3];          // [1]: x 1 / log2 of the fog's far distance, y the normalising factor
    float4 FogColor;
    float4 FogCameraCenterOffset;
    float4 FogShadowMapResolutions;
};

cbuffer FogMaterial : register(b2) // cPSMaterial of the game's fog pass
{
    float4 FogMedium[8];
};

cbuffer SunScene : register(b5) // cPSScene of the sun pass
{
    float4x4 m_projectionView;
    float4x4 m_projection;
    float4 m_view[4];               // Rows: view to world space (the camera's position in w)
    float4 m_shadowProjection[4];   // Rows: view position to the first cascade
    float4 m_cascades[4];           // m_shadowProjection2: columns x, y, z place cascades 1 to 3 (offset, scale in row 3)
    float4 m_eyepos;                // The camera in the world, metres, y up
    float4 m_projectionParam;
    float4 m_viewportSize;
    float4 m_exposure;
    float4 m_fogParam[3];
    float4 m_fogColor;
    float4 m_cameraCenterOffset;
    float4 m_shadowMapResolutions;
};

cbuffer SunLight : register(b6) // cPSLight of the sun pass: [0] towards the sun, view space; [1] its colour
{
    float4 m_lightParams[11];
};

static const uint Slices = 128;

// The view-space direction (z 1) of a place in a slice tile, in texels, as the game's readers sample
// the fog: uv = clip x (0.014648, 0.123047) + (0.015625, 0.125) across the 32 x 4 tiles.
float3 ColumnDirection(float2 texel)
{
    float2 clip = ((texel + 0.5) / TileSize - 0.5) / float2(0.46875, 0.4921875);
    return float3(clip * FogProjectionParam.xy, 1);
}

// Slice depths in the view run from 1 m to the far distance, evenly in log2.
float SliceDepth(float slice)
{
    return exp2(slice / (Slices - 1) / FogParam[1].x);
}

// A view-space direction in the world, y up.
float3 ToWorld(float3 direction)
{
    return float3(dot(m_view[0].xyz, direction), dot(m_view[1].xyz, direction), dot(m_view[2].xyz, direction));
}
)"

// Earth's atmosphere as Hillaire's reference sets it up, in km, and its lookup tables. The
// atmosphere's frame has z up with the planet's centre at the origin; the game's world has y up.
#define PHANTOM_FX_ATMOSPHERE R"(
#ifndef LINEAR_CLAMP_SAMPLER
#define LINEAR_CLAMP_SAMPLER s0
#endif
SamplerState LinearClamp : register(LINEAR_CLAMP_SAMPLER);
Texture2D<float4> TransmittanceLut : register(t4);       // 256x64: transmittance to the atmosphere's top
Texture2D<float4> MultipleScatteringLut : register(t5);  // 32x32: light scattered more than once, per unit of sunlight
Texture2D<float4> SkyParameters : register(t6);          // SkyParameters' output, see there
Texture2D<float4> SkyViewLut : register(t7);             // 192x108: the sky around the camera, per unit of sunlight

static const float Pi = 3.14159265;
static const float BottomRadius = 6360, TopRadius = 6460;
static const float3 RayleighScattering = float3(0.005802, 0.013558, 0.033100);
static const float RayleighExpScale = -1.0 / 8.0;
static const float3 MieScattering = 0.003996, MieExtinction = 0.004440;
static const float MieExpScale = -1.0 / 1.2;
static const float MiePhaseG = 0.8;
static const float3 OzoneExtinction = float3(0.000650, 0.001881, 0.000085);
static const float PlanetRadiusOffset = 0.01;
// Afghanistan's and Africa's ground: sand, rock and dry grass reflect about this much.
static const float GroundAlbedo = 0.3;
static const float2 TransmittanceSize = float2(256, 64);
static const float MultipleScatteringSize = 32;
static const float2 SkyViewSize = float2(192, 108);

// SkyParameters' texels.
static const int SunTexel = 0;      // rgb the sunlight at the atmosphere's top, in the game's exposed units; a the camera's height, km
static const int DirectionTexel = 1; // xyz towards the sun, in the world
static const int AmbientTexel = 2;   // rgb the sky's mean light, for the fog
static const int FogTexel = 3;       // rgb the height fog's albedo, a its extinction at the camera, 1/m
static const int FogShapeTexel = 4;  // x its height scale, m; y its phase's g; z the camera's height in the world, m
static const int SourceTexel = 5;    // For the log: x the game's sunlight on the ground, y its sky's, z the air's transmittance to the sun
static const int OvercastTexel = 6;  // rgb the light of the game's cloud deck over the sky, a how much of the sky it covers
static const uint SkyParameterTexels = 7;

float3 ToAtmosphere(float3 world)
{
    return float3(world.x, world.z, world.y);
}

float RaySphereIntersectNearest(float3 origin, float3 direction, float3 centre, float radius)
{
    float a = dot(direction, direction);
    float3 offset = origin - centre;
    float b = 2 * dot(direction, offset);
    float c = dot(offset, offset) - radius * radius;
    float delta = b * b - 4 * a * c;
    if (delta < 0 || a == 0) return -1;

    float near = (-b - sqrt(delta)) / (2 * a), far = (-b + sqrt(delta)) / (2 * a);
    if (near < 0 && far < 0) return -1;
    if (near < 0) return max(0, far);
    if (far < 0) return max(0, near);

    return max(0, min(near, far));
}

float FromUnitToSubUvs(float u, float resolution) { return (u + 0.5 / resolution) * (resolution / (resolution + 1)); }
float FromSubUvsToUnit(float u, float resolution) { return (u - 0.5 / resolution) * (resolution / (resolution - 1)); }

float2 TransmittanceUv(float viewHeight, float viewZenithCos)
{
    float H = sqrt(max(0, TopRadius * TopRadius - BottomRadius * BottomRadius));
    float rho = sqrt(max(0, viewHeight * viewHeight - BottomRadius * BottomRadius));
    float discriminant = viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1) + TopRadius * TopRadius;
    float d = max(0, -viewHeight * viewZenithCos + sqrt(discriminant));
    float dMin = TopRadius - viewHeight, dMax = rho + H;

    return float2((d - dMin) / (dMax - dMin), rho / H);
}

float3 TransmittanceToTop(float viewHeight, float viewZenithCos)
{
    return TransmittanceLut.SampleLevel(LinearClamp, TransmittanceUv(viewHeight, viewZenithCos), 0).rgb;
}

// The sunlight's share through the air at a height: none once the planet hides the sun.
float3 SunTransmittance(float viewHeight, float sunZenithCos)
{
    float horizonCos = -sqrt(max(1 - BottomRadius * BottomRadius / (viewHeight * viewHeight), 0));
    return sunZenithCos >= horizonCos ? TransmittanceToTop(viewHeight, sunZenithCos) : 0;
}

float3 MultipleScattering(float viewHeight, float sunZenithCos)
{
    float2 uv = saturate(float2(sunZenithCos * 0.5 + 0.5, (viewHeight - BottomRadius) / (TopRadius - BottomRadius)));
    uv = float2(FromUnitToSubUvs(uv.x, MultipleScatteringSize), FromUnitToSubUvs(uv.y, MultipleScatteringSize));
    return MultipleScatteringLut.SampleLevel(LinearClamp, uv, 0).rgb;
}

float2 SkyViewUv(bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight)
{
    float horizon = sqrt(viewHeight * viewHeight - BottomRadius * BottomRadius);
    float beta = acos(horizon / viewHeight);
    float zenithHorizonAngle = Pi - beta;
    float2 uv;
    if (!intersectGround)
    {
        float coord = 1 - sqrt(1 - acos(viewZenithCos) / zenithHorizonAngle);
        uv.y = coord * 0.5;
    }
    else
    {
        float coord = sqrt((acos(viewZenithCos) - zenithHorizonAngle) / beta);
        uv.y = coord * 0.5 + 0.5;
    }
    uv.x = sqrt(-lightViewCos * 0.5 + 0.5);

    return float2(FromUnitToSubUvs(uv.x, SkyViewSize.x), FromUnitToSubUvs(uv.y, SkyViewSize.y));
}

// The sky towards `direction` from the camera, both in the world, per unit of sunlight. Below the
// horizon it is the horizon, as the game's sky does: the game's ground and sea cover the planet there,
// and where they stop short of it (the sea at Mother Base) the planet's dark air showed as a band.
float3 SkyLight(float3 direction, float3 towardsSun, float cameraHeight)
{
    float3 d = ToAtmosphere(direction), sun = ToAtmosphere(towardsSun);
    float viewHeight = BottomRadius + cameraHeight;
    float3 position = float3(0, 0, viewHeight);
    float3 side = cross(float3(0, 0, 1), d);
    side = dot(side, side) > 1e-8 ? normalize(side) : float3(1, 0, 0);
    float3 forward = normalize(cross(side, float3(0, 0, 1)));
    float2 sunOnPlane = float2(dot(sun, forward), dot(sun, side));
    float lightViewCos = dot(sunOnPlane, sunOnPlane) > 1e-8 ? normalize(sunOnPlane).x : 1;
    bool intersectGround = RaySphereIntersectNearest(position, d, 0, BottomRadius) >= 0;
    float2 uv = SkyViewUv(intersectGround, d.z, lightViewCos, viewHeight);
    // The table's upper half is the sky, its last row the horizon.
    uv.y = min(uv.y, (SkyViewSize.y / 2 - 0.5) / SkyViewSize.y);

    return SkyViewLut.SampleLevel(LinearClamp, uv, 0).rgb;
}

struct Medium
{
    float3 scattering, extinction, rayleigh, mie;
};

Medium SampleMedium(float height) // km above the ground
{
    float rayleigh = exp(RayleighExpScale * height), mie = exp(MieExpScale * height);
    float ozone = saturate(height < 25 ? height / 15 - 2.0 / 3 : -height / 15 + 8.0 / 3);
    Medium medium;
    medium.rayleigh = rayleigh * RayleighScattering;
    medium.mie = mie * MieScattering;
    medium.scattering = medium.rayleigh + medium.mie;
    medium.extinction = medium.rayleigh + mie * MieExtinction + ozone * OzoneExtinction;

    return medium;
}

float RayleighPhase(float cosTheta)
{
    return 3 / (16 * Pi) * (1 + cosTheta * cosTheta);
}

// Cornette-Shanks; cosTheta between the way towards the light and the way the eye looks.
float MiePhase(float g, float cosTheta)
{
    float k = 3 / (8 * Pi) * (1 - g * g) / (2 + g * g);
    return k * (1 + cosTheta * cosTheta) / pow(max(1 + g * g - 2 * g * cosTheta, 1e-6), 1.5);
}

struct Scattering
{
    float3 light, multipleAsOne;
};

// Hillaire's IntegrateScatteredLuminance, per unit of sunlight; with `ground`, light the ground
// reflects where the ray meets it.
Scattering Integrate(float3 position, float3 direction, float3 towardsSun, float samples, bool variableSamples, bool phases, bool multiple,
    bool ground)
{
    Scattering result = (Scattering)0;
    float tBottom = RaySphereIntersectNearest(position, direction, 0, BottomRadius);
    float tTop = RaySphereIntersectNearest(position, direction, 0, TopRadius);
    float tMax = 0;
    if (tBottom < 0)
    {
        if (tTop < 0) return result;
        tMax = tTop;
    }
    else if (tTop > 0)
        tMax = min(tTop, tBottom);

    float count = samples, countFloor = samples, tMaxFloor = tMax;
    if (variableSamples)
    {
        count = lerp(4, 14, saturate(tMax * 0.01));
        countFloor = floor(count);
        tMaxFloor = tMax * countFloor / count;
    }

    float cosTheta = dot(towardsSun, direction);
    float miePhase = MiePhase(MiePhaseG, cosTheta), rayleighPhase = RayleighPhase(cosTheta);
    float3 light = 0, throughput = 1;
    float t = 0;
    for (float s = 0; s < count; s += 1)
    {
        float dt;
        if (variableSamples)
        {
            float t0 = s / countFloor, t1 = (s + 1) / countFloor;
            t0 = tMaxFloor * t0 * t0;
            t1 = t1 * t1 > 1 ? tMax : tMaxFloor * t1 * t1;
            t = t0 + (t1 - t0) * 0.3;
            dt = t1 - t0;
        }
        else
        {
            float next = tMax * (s + 0.3) / count;
            dt = next - t;
            t = next;
        }

        float3 p = position + t * direction;
        float height = length(p);
        float3 up = p / height;
        Medium medium = SampleMedium(height - BottomRadius);
        float3 sampleTransmittance = exp(-medium.extinction * dt);
        float sunZenithCos = dot(towardsSun, up);
        float3 toSun = TransmittanceToTop(height, sunZenithCos);
        float3 phaseScattering = phases ? medium.mie * miePhase + medium.rayleigh * rayleighPhase : medium.scattering / (4 * Pi);
        float earthShadow = RaySphereIntersectNearest(p, towardsSun, PlanetRadiusOffset * up, BottomRadius) >= 0 ? 0 : 1;
        float3 multipleLight = multiple ? MultipleScattering(height, sunZenithCos) : 0;

        float3 inScattered = earthShadow * toSun * phaseScattering + multipleLight * medium.scattering;
        result.multipleAsOne += throughput * (medium.scattering - medium.scattering * sampleTransmittance) / medium.extinction;
        light += throughput * (inScattered - inScattered * sampleTransmittance) / medium.extinction;
        throughput *= sampleTransmittance;
    }

    if (ground && tMax == tBottom && tBottom > 0)
    {
        float3 p = position + tBottom * direction;
        float3 up = normalize(p);
        light += TransmittanceToTop(length(p), dot(towardsSun, up)) * throughput * saturate(dot(up, towardsSun)) * GroundAlbedo / Pi;
    }

    result.light = light;
    return result;
}
)"

// The transmittance from a height towards the atmosphere's top, by height and zenith angle.
inline constexpr char AtmosphereTransmittance[] = PHANTOM_FX_ATMOSPHERE R"(
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float2 uv = (id.xy + 0.5) / TransmittanceSize;
    float H = sqrt(TopRadius * TopRadius - BottomRadius * BottomRadius);
    float rho = H * uv.y;
    float viewHeight = sqrt(rho * rho + BottomRadius * BottomRadius);
    float dMin = TopRadius - viewHeight, dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    float viewZenithCos = d == 0 ? 1 : clamp((H * H - rho * rho - d * d) / (2 * viewHeight * d), -1, 1);

    float3 position = float3(0, 0, viewHeight);
    float3 direction = float3(0, sqrt(1 - viewZenithCos * viewZenithCos), viewZenithCos);
    float tMax = RaySphereIntersectNearest(position, direction, 0, TopRadius);
    float tBottom = RaySphereIntersectNearest(position, direction, 0, BottomRadius);
    if (tBottom >= 0) tMax = min(tMax, tBottom);

    static const float Samples = 40;
    float3 opticalDepth = 0;
    float t = 0;
    for (float s = 0; s < Samples; s += 1)
    {
        float next = tMax * (s + 0.3) / Samples;
        opticalDepth += SampleMedium(length(position + next * direction) - BottomRadius).extinction * (next - t);
        t = next;
    }
    Output[id.xy] = float4(exp(-opticalDepth), 1);
}
)";

// Hillaire's multiple scattering lookup table: one thread group per texel, 64 directions each.
inline constexpr char AtmosphereMultipleScattering[] = PHANTOM_FX_ATMOSPHERE R"(
RWTexture2D<float4> Output : register(u0);
groupshared float3 MultipleAsOneShared[64];
groupshared float3 LightShared[64];

[numthreads(1, 1, 64)]
void main(uint3 id : SV_DispatchThreadID)
{
    float2 uv = (id.xy + 0.5) / MultipleScatteringSize;
    uv = float2(FromSubUvsToUnit(uv.x, MultipleScatteringSize), FromSubUvsToUnit(uv.y, MultipleScatteringSize));
    float sunZenithCos = uv.x * 2 - 1;
    float3 towardsSun = float3(0, sqrt(saturate(1 - sunZenithCos * sunZenithCos)), sunZenithCos);
    float viewHeight = BottomRadius + saturate(uv.y + PlanetRadiusOffset) * (TopRadius - BottomRadius - PlanetRadiusOffset);
    float3 position = float3(0, 0, viewHeight);

    float i = 0.5 + id.z / 8, j = 0.5 + id.z % 8;
    float theta = 2 * Pi * i / 8, phi = acos(1 - 2 * j / 8);
    float3 direction = float3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));
    Scattering result = Integrate(position, direction, towardsSun, 20, false, false, false, true);
    MultipleAsOneShared[id.z] = result.multipleAsOne * 4 * Pi / 64;
    LightShared[id.z] = result.light * 4 * Pi / 64;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint half = 32; half > 0; half /= 2)
    {
        if (id.z < half)
        {
            MultipleAsOneShared[id.z] += MultipleAsOneShared[id.z + half];
            LightShared[id.z] += LightShared[id.z + half];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (id.z > 0) return;

    // Every order of scattering after the first, as a geometric series.
    float3 multipleAsOne = MultipleAsOneShared[0] / (4 * Pi);
    float3 secondOrder = LightShared[0] / (4 * Pi);
    Output[id.xy] = float4(secondOrder / (1 - multipleAsOne), 1);
}
)";

// One thread: what the sky, the physical fog and the sky draw share this frame, from the sun pass's
// constants and the game's fog pass's.
inline constexpr char SkyParameterPass[] = PHANTOM_FX_FOG_INPUTS PHANTOM_FX_ATMOSPHERE R"(
cbuffer SkyMaterial : register(b8) // cPSMaterial of the last sky draw: [3] towards the sun in the world, and the sun its sky is lit by
{
    float4 SkyMaterial[8];
};

cbuffer SkyObject : register(b9) // cPSObject of the last sky draw: [10].x how overcast it draws the sky
{
    float4 SkyObject[13];
};

RWTexture2D<float4> Output : register(u0);

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// The game's fog has Schlick's phase; its k stands for the g of Henyey-Greenstein's that solves
// k = 1.55 g - 0.55 g^3 (k 0.96 is g 0.8). Taken as g it narrows the sun's glow to a disc.
float SchlickToG(float k)
{
    k = clamp(k, 0, 0.99);
    float g = 0.5;
    [unroll] for (int step = 0; step < 6; ++step) g -= (1.55 * g - 0.55 * g * g * g - k) / (1.55 - 1.65 * g * g);

    return saturate(g);
}

[numthreads(1, 1, 1)]
void main()
{
    // The game lights the ground with the sun at least 26 degrees up, and its sky with the sun where it
    // is, brighter as it sets to make up for the longer way through the air: the sky's direction and
    // light belong together.
    float3 towardsSun = SkySun ? normalize(SkyMaterial[3].xyz) : normalize(ToWorld(m_lightParams[0].xyz));
    float cameraHeight = max(m_eyepos.y * 0.001, 0.001);
    // The game lights its sky with its own sun, about four times the sunlight it lights the ground
    // with: the sky's sun above the air, exposed and at most 1000, as the game's sky takes it. Without
    // a sky draw with the sun, the ground's: it reaches the camera through the air, and the game lights
    // a surface with its albedo times it, where a physical one takes albedo / pi.
    float3 transmittance = TransmittanceToTop(BottomRadius + cameraHeight, towardsSun.y);
    float groundSunlight = Luminance(m_lightParams[1].xyz * m_exposure.z);
    float skySunlight = min(SkyMaterial[3].w * m_exposure.z, 1000);
    float sunlight = SkySun ? skySunlight : Pi * groundSunlight / max(Luminance(transmittance), 0.002);

    // The game's weather draws a grey cloud deck over its sky (its sky draws' "cloudy" share: 0 on a
    // clear day, 1 in rain), and lights the ground under it with almost no sun. The deck takes that
    // share of the blue sky and of the sun on the air and the fog, and lights them itself, with what
    // the game's fog adds up to far away: (sun x mean phase x betaM + sky x betaU) / extinction, the
    // game's own light for that weather (in rain at dusk about 0.18 grey, as its sky draws).
    float overcast = SkySun ? saturate(SkyObject[10].x) : 0;
    float3 betaM = max(FogMedium[2].xyz, 0), betaR = max(FogMedium[3].xyz, 0);
    float betaU = max(FogMedium[3].w, 0), mieRatio = FogMedium[0].y;
    float meanMiePhase = (1 - mieRatio) / (4 * Pi) + mieRatio;
    float3 deckLight = (FogMedium[0].w * (meanMiePhase * betaM + betaR / (4 * Pi)) + FogMedium[1].xyz * betaU)
        / max(betaM + betaR + betaU, 1e-6) + max(FogMedium[7].xyz, 0);

    // The game's fog thins the view to e^(-density x od(1 km)) over a kilometre, a clear day in
    // Afghanistan to a fifth: it fades the distant ground, and the game's sky leaves most of it out.
    // As a medium that thick it would grey the whole sky, so at no haze the height fog takes a tenth
    // of it on a clear day, rising to all of it at ten times that (fog, sandstorms), less what the
    // air does. Haze raises it to all of it in any weather, in a layer as deep as dust (1 km): the
    // game's haze does not thin with height, and real dust pales the sky. Under the cloud deck it is
    // rain and mist, which the air does not make: all of it too.
    float opticalDepth = exp2(log2(max(1000 - FogMedium[0].z, 0)) * FogMedium[1].w);
    float gameExtinction = FogMedium[0].x * opticalDepth / 1000;
    float share = lerp(min(gameExtinction / 0.015, 1), 1, max(saturate(Haze), overcast));
    float fogExtinction = max(gameExtinction * share - 0.000015, 0);
    float fogScaleHeight = lerp(300, 1000, saturate(Haze));
    float3 albedo = 0.9 * betaM / max(max(betaM.r, max(betaM.g, betaM.b)), 1e-6);
    if (all(betaM <= 0)) albedo = 0.9;

    Output[uint2(SunTexel, 0)] = float4((sunlight * (1 - overcast)).xxx, cameraHeight);
    Output[uint2(DirectionTexel, 0)] = float4(towardsSun, 0);
    Output[uint2(FogTexel, 0)] = float4(albedo, fogExtinction);
    Output[uint2(FogShapeTexel, 0)] = float4(fogScaleHeight, SchlickToG(FogMedium[2].w), m_eyepos.y, 0);
    Output[uint2(SourceTexel, 0)] = float4(groundSunlight, SkySun ? skySunlight : 0, Luminance(transmittance), gameExtinction);
    Output[uint2(OvercastTexel, 0)] = float4(deckLight * overcast, overcast);
}
)";

// The sky around the camera, per unit of sunlight (Hillaire's sky-view lookup table).
inline constexpr char SkyView[] = PHANTOM_FX_ATMOSPHERE R"(
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(SkyViewSize))) return;

    float cameraHeight = SkyParameters.Load(int3(SunTexel, 0, 0)).a;
    float3 sunWorld = ToAtmosphere(SkyParameters.Load(int3(DirectionTexel, 0, 0)).xyz);
    float viewHeight = BottomRadius + cameraHeight;

    float2 uv = (id.xy + 0.5) / SkyViewSize;
    uv = float2(FromSubUvsToUnit(uv.x, SkyViewSize.x), FromSubUvsToUnit(uv.y, SkyViewSize.y));
    float beta = acos(sqrt(viewHeight * viewHeight - BottomRadius * BottomRadius) / viewHeight);
    float zenithHorizonAngle = Pi - beta;
    float viewZenithCos;
    if (uv.y < 0.5)
    {
        float coord = 1 - 2 * uv.y;
        viewZenithCos = cos(zenithHorizonAngle * (1 - coord * coord));
    }
    else
    {
        float coord = uv.y * 2 - 1;
        viewZenithCos = cos(zenithHorizonAngle + beta * coord * coord);
    }
    float lightViewCos = -(uv.x * uv.x * 2 - 1);

    float sunZenithCos = sunWorld.z;
    float3 towardsSun = normalize(float3(sqrt(saturate(1 - sunZenithCos * sunZenithCos)), 0, sunZenithCos));
    float viewZenithSin = sqrt(saturate(1 - viewZenithCos * viewZenithCos));
    float3 direction = float3(viewZenithSin * lightViewCos, viewZenithSin * sqrt(saturate(1 - lightViewCos * lightViewCos)), viewZenithCos);
    Output[id.xy] = float4(Integrate(float3(0, 0, viewHeight), direction, towardsSun, 30, true, true, true, false).light, 1);
}
)";

// The mean light from all around, which lights the height fog: the sky above, and the ground below,
// lit by the sun through the air and by the sky; under the game's cloud deck, its light.
inline constexpr char SkyAmbient[] = PHANTOM_FX_ATMOSPHERE R"(
RWTexture2D<float4> Output : register(u0); // SkyParameters
groupshared float4 Sums[64];

[numthreads(8, 8, 1)]
void main(uint3 id : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    float4 sun = Output[uint2(SunTexel, 0)];
    float3 towardsSun = Output[uint2(DirectionTexel, 0)].xyz;
    float zenithCos = (id.y + 0.5) / 8, zenithSin = sqrt(1 - zenithCos * zenithCos);
    float azimuth = 2 * Pi * (id.x + 0.5) / 8;
    float3 direction = float3(zenithSin * cos(azimuth), zenithCos, zenithSin * sin(azimuth));
    Sums[index] = float4(SkyLight(direction, towardsSun, sun.a), 1);
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint half = 32; half > 0; half /= 2)
    {
        if (index < half) Sums[index] += Sums[index + half];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index > 0) return;

    float3 sky = sun.rgb * Sums[0].rgb / Sums[0].a;
    float3 sunlight = sun.rgb * TransmittanceToTop(BottomRadius + sun.a, towardsSun.y) * saturate(towardsSun.y);
    float3 ground = GroundAlbedo * (sunlight / Pi + sky);
    // The deck's light is the game's fog light, which already holds the ground's.
    float3 deck = Output[uint2(OvercastTexel, 0)].rgb;
    Output[uint2(AmbientTexel, 0)] = float4(0.5 * (sky + ground) + deck, 0);
}
)";

// One thread per froxel: the sun's share in it this frame, from points spread through it, at a new
// place in it each frame, the same along the whole view ray (Hillaire, "Physically Based and Unified
// Volumetric Rendering in Frostbite", 2015). FogMedia adds the last frames' share.
inline constexpr char FogVisibility[] = PHANTOM_FX_FOG_INPUTS R"(
Texture2D<float> ShadowTexture : register(t1); // The sun's cascades
SamplerComparisonState ShadowSampler : register(s1);
RWTexture3D<float> Visibility : register(u0);

// A view-space point placed in cascade `level` (0 to 3): x and y in -1 to 1 inside it, z towards the sun.
float3 CascadePosition(float3 shadowPosition, int level)
{
    if (level == 0) return shadowPosition;

    int column = level - 1;
    return float3(m_cascades[0][column], m_cascades[1][column], m_cascades[2][column]) + shadowPosition * m_cascades[3][column];
}

// The share of the sun reaching a view-space point: one comparison in the first cascade that holds
// it, as the sun pass picks it, faded to lit towards the end of the shadow range as the sun pass fades.
float SunVisibility(float3 viewPosition)
{
    float4 position = float4(viewPosition, 1);
    float resolution = m_shadowMapResolutions.x;
    float3 shadowPosition = float3(dot(position, m_shadowProjection[0]), dot(position, m_shadowProjection[1]),
        dot(position, m_shadowProjection[2]));
    float area = 1 - 8 / resolution;
    area *= area;

    int level = 3;
    float3 placed = CascadePosition(shadowPosition, 3);
    [unroll] for (int candidate = 2; candidate >= 0; --candidate)
    {
        float3 inCascade = CascadePosition(shadowPosition, candidate);
        if (all(inCascade.xy * inCascade.xy <= area) && inCascade.z >= -1)
        {
            level = candidate;
            placed = inCascade;
        }
    }

    float halfTexel = 0.5 / resolution;
    float row = saturate(level - 1);
    float2 uv = min(float2(placed.x * 0.5 + 0.5, 0.5 - placed.y * 0.5) * 0.5, 1 - halfTexel * 2) + float2(level / 2.0 - row, row * 0.5) + halfTexel;
    float compareDepth = max(m_cascades[0].w * (placed.z + 1), 0.000001);
    float shadow = 1 - ShadowTexture.SampleCmpLevelZero(ShadowSampler, uv, compareDepth);

    float3 fromMaxLevel = viewPosition - float3(0, 0, m_lightParams[8].x);
    float pastShadowStart = max(length(fromMaxLevel) - m_lightParams[8].y, 0);
    float fade = saturate(pastShadowStart * m_lightParams[6].x) * (fromMaxLevel.z >= 0 ? 1.0 : 0.0);
    fade *= fade;

    return saturate((1 - shadow) * fade + shadow - m_lightParams[6].w);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= TileSize)) return;

    float nearDepth = id.z == 0 ? 0 : SliceDepth(id.z - 1);
    float farDepth = SliceDepth(id.z);
    float3 place = ColumnDirection(id.xy + Jitter);
    float visibility = 0;
    for (uint i = 0; i < ShadowSamples; ++i)
        visibility += SunVisibility(place * lerp(nearDepth, farDepth, (i + DepthJitter) / ShadowSamples));
    Visibility[id] = visibility / ShadowSamples;
}
)";

// One thread per froxel: what its depth slice adds to the fog. In the game's look, the fog its slice
// adds over the slice before it (rgb) and the transmittance at its depth (a), by the game's fog
// pass's formula (docs/DEVELOPMENT.md, "Volumetric fog") with the sun's share shadowed; without a
// shadow it gives the game's fog. In the physical look, the light the slice scatters towards the
// camera (rgb) and its transmittance (a), of the air and the height fog, and of the height fog alone
// in FogOnly.
inline constexpr char FogMedia[] = PHANTOM_FX_FOG_INPUTS PHANTOM_FX_ATMOSPHERE R"(
cbuffer FogObject : register(b3) // cPSObject of the game's fog pass: [9] its camera, [10] towards the sun
{
    float4 FogObjectParams[13];
};

cbuffer FogSystem : register(b4) // cPSSystem of the game's fog pass
{
    float4 SystemParam, SystemRenderInfo, SystemRenderBuffer, SystemDominantLightDirection;
};

cbuffer LastSunScene : register(b7) // The last frame's SunScene
{
    float4x4 LastProjectionView;
    float4x4 LastProjection;
    float4 LastView[4];
};

Texture2D<float4> RainTexture : register(t0);      // The fog pass's top-down rain cover
Texture3D<float> VisibilityHistory : register(t2); // The last frame's Visibility
Texture3D<float> CurrentVisibility : register(t3); // FogVisibility's
RWTexture3D<float4> Media : register(u0);
RWTexture3D<float> Visibility : register(u1);      // The sun's share in each froxel, over frames
RWTexture3D<float4> FogOnly : register(u2);

// Where a view-space point of this frame lay in the last frame's froxels, as a uvw of the
// visibility volume; outside 0 to 1 when it was out of view. m_view takes view to world space.
float3 LastFroxel(float3 viewPosition)
{
    float3 world = ToWorld(viewPosition) + float3(m_view[0].w, m_view[1].w, m_view[2].w);
    float3 offset = world - float3(LastView[0].w, LastView[1].w, LastView[2].w);
    float3 last = offset.x * LastView[0].xyz + offset.y * LastView[1].xyz + offset.z * LastView[2].xyz;
    if (last.z <= 0) return -1;

    float2 clip = last.xy / (last.z * FogProjectionParam.xy);
    float slice = log2(last.z) * FogParam[1].x * (Slices - 1);
    return float3(clip * float2(0.46875, 0.4921875) + 0.5, (slice + 0.5) / Slices);
}

// The sun's share in the froxel over frames: the last frames' share where the froxel was in view,
// held within what this frame's neighbouring froxels show, so it follows a moving shadow at once
// and stays smooth where nothing moves (Karis, "High Quality Temporal Supersampling", 2014).
float BlendedVisibility(uint3 id)
{
    float current = CurrentVisibility[id];
    float3 last = LastFroxel(ColumnDirection(id.xy) * SliceDepth(id.z));
    if (HistoryWeight <= 0 || any(last < 0 || last > 1)) return current;

    int3 size = int3(TileSize, Slices);
    float low = current, high = current;
    [unroll] for (int z = -1; z <= 1; ++z)
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                float neighbour = CurrentVisibility[clamp(int3(id) + int3(x, y, z), 0, size - 1)];
                low = min(low, neighbour);
                high = max(high, neighbour);
            }

    float history = clamp(VisibilityHistory.SampleLevel(LinearClamp, last, 0), low, high);
    return lerp(current, history, HistoryWeight);
}

// The fog pass's distance from its camera to a view-space point, and the direction to it.
float FogDistance(float3 viewPosition, out float3 direction)
{
    float4 position = float4(viewPosition, 1);
    float3 placed = float3(dot(position, FogPlacement[0]), dot(position, FogPlacement[1]), dot(position, FogPlacement[2]));
    float3 ray = placed - FogObjectParams[9].xyz;
    float distance = sqrt(dot(ray, ray));
    direction = ray / max(distance, 0.0001);

    return distance;
}

float OpticalDepth(float distance)
{
    return exp2(log2(max(distance - FogMedium[0].z, 0)) * FogMedium[1].w);
}

// How open to the rain the camera stands, as the fog pass reads it: 1 in the open.
float RainAlpha()
{
    float3 position = float3(FogMedium[4].x - FogObjectParams[9].x, FogObjectParams[9].z + FogMedium[4].y, FogMedium[4].w - FogObjectParams[9].y);
    float3 screen = float3(position.xy * 0.03 * float2(0.5, -0.5) + 0.5, 1 - saturate(position.z * 0.02));
    float4 cover = RainTexture.SampleLevel(LinearClamp, screen.xy - SystemRenderBuffer.zw * 0.5, 0).xzyw;
    float2 t = saturate((cover.xy + 0.001 - screen.z) / 0.001);
    float2 alpha = 1 - t + cover.zw * t;

    return alpha.x * alpha.y;
}

// The game's look: its formula, the sun's share shadowed.
float4 GameMedia(uint3 id, float visibility)
{
    float3 column = ColumnDirection(id.xy);
    float nearDepth = id.z == 0 ? 0 : SliceDepth(id.z - 1);
    float farDepth = SliceDepth(id.z);
    float3 direction, unused;
    float farDistance = FogDistance(column * farDepth, direction);
    float nearDistance = id.z == 0 ? 0 : FogDistance(column * nearDepth, unused);

    float representativeDensity = FogMedium[0].x, mieRatio = FogMedium[0].y, sunIntensity = FogMedium[0].w;
    float mieK = FogMedium[2].w * RainAlpha();
    float3 skyLuminance = FogMedium[1].xyz, betaM = FogMedium[2].xyz, betaR = FogMedium[3].xyz, betaU = FogMedium[3].www;
    float mu = dot(FogObjectParams[10].xyz, direction);
    float miePhase = (1 - mieK * mieK) * 0.0795775 / ((1 - mieK * mu) * (1 - mieK * mu));
    miePhase = miePhase * (1 - mieRatio) + mieRatio;
    float rayleighPhase = (mu * mu + 1) * 0.0596831;
    float3 sun = (miePhase * betaM + rayleighPhase * betaR) * sunIntensity;
    float3 ambient = skyLuminance * betaU;
    float3 extinction = max(betaM + betaR + betaU, 0.000001);

    // The game's colour is (sun + ambient) / extinction x (1 - e^-extinction x optical depth); each
    // slice adds its part of that, so the slices add up to it where nothing shadows the sun.
    float3 before = id.z == 0 ? 1 : exp(-extinction * OpticalDepth(nearDistance));
    float3 after = exp(-extinction * OpticalDepth(farDistance));
    float3 added = (sun * visibility + ambient) / extinction * (before - after);
    return float4(added, exp(-representativeDensity * OpticalDepth(farDistance)));
}

// The physical look: the air (Rayleigh, Mie, ozone) at the froxel's height, lit by the sun through
// the air above it and by the air's multiple scattering, and the height fog, lit by the sun and the
// sky's mean light; integrated over the slice as Frostbite does, (S - S e^-extinction x d) / extinction.
float4 PhysicalMedia(uint3 id, float visibility, out float4 fogOnly)
{
    float3 column = ColumnDirection(id.xy);
    float nearDepth = id.z == 0 ? 0 : SliceDepth(id.z - 1);
    float farDepth = SliceDepth(id.z);
    float columnLength = length(column);
    float3 viewDirection = column / columnLength;
    float length_ = (farDepth - nearDepth) * columnLength; // metres
    float3 middle = column * (nearDepth + farDepth) * 0.5;

    float4 sun = SkyParameters.Load(int3(SunTexel, 0, 0));
    float3 ambient = SkyParameters.Load(int3(AmbientTexel, 0, 0)).rgb;
    float4 fog = SkyParameters.Load(int3(FogTexel, 0, 0));
    float4 fogShape = SkyParameters.Load(int3(FogShapeTexel, 0, 0));
    float3 towardsSunWorld = SkyParameters.Load(int3(DirectionTexel, 0, 0)).xyz;

    float height = fogShape.z + dot(ToWorld(middle), float3(0, 1, 0)); // metres in the world
    float altitude = max(sun.a + (height - fogShape.z) * 0.001, 0);   // km above the ground
    Medium air = SampleMedium(altitude);
    float fogDensity = fog.a * min(exp(-(height - fogShape.z) / fogShape.x), 4);

    // Lit as the game lights the ground: from the sun pass's sun, at least 26 degrees up, which casts
    // the shadow map, so a low sun's glow never shows through the hills in front of it; the light is
    // the sky's sun's through the air, so it reddens and fades as the real sun sets.
    // The height fog also dims the sunlight that reaches the froxel: its optical depth towards the sun,
    // density x scale height / sin(elevation), the sine at least 0.01 (around the Earth's curve a 1 km
    // layer is about 100 km long at the horizon). Without it dense haze glowed white towards a low sun.
    float cosTheta = dot(normalize(m_lightParams[0].xyz), viewDirection);
    float hazeToSun = exp(-fogDensity * fogShape.x / max(towardsSunWorld.y, 0.01));
    float3 toSun = SunTransmittance(BottomRadius + altitude, towardsSunWorld.y) * hazeToSun * visibility;
    float3 multiple = MultipleScattering(BottomRadius + altitude, towardsSunWorld.y);
    // The game's cloud deck takes its share of the sun (in sun.rgb) and lights the air from all around.
    float3 deck = SkyParameters.Load(int3(OvercastTexel, 0, 0)).rgb;
    float3 airLight = sun.rgb * (toSun * (air.rayleigh * RayleighPhase(cosTheta) + air.mie * MiePhase(MiePhaseG, cosTheta))
        + multiple * air.scattering) + deck * air.scattering;
    float3 fogScattering = fog.rgb * fogDensity;
    float3 fogLight = fogScattering * (sun.rgb * toSun * MiePhase(fogShape.y, cosTheta) + ambient);

    // The air's coefficients are per km.
    float3 airExtinction = air.extinction * 0.001;
    float3 extinction = max(airExtinction + fogDensity, 1e-9);
    float3 transmittance = exp(-extinction * length_);
    float3 light = (airLight * 0.001 + fogLight) * (1 - transmittance) / extinction;

    float fogExtinction = max(fogDensity, 1e-9);
    float fogTransmittance = exp(-fogExtinction * length_);
    fogOnly = float4(fogLight * (1 - fogTransmittance) / fogExtinction, fogTransmittance);

    return float4(light, dot(transmittance, 1.0 / 3));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= TileSize)) return;

    float visibility = Shadowed ? BlendedVisibility(id) : 1;
    Visibility[id] = visibility;

    float4 fogOnly = 0;
    Media[id] = Physical ? PhysicalMedia(id, visibility, fogOnly) : GameMedia(id, visibility);
    FogOnly[id] = fogOnly;
}
)";

// One thread per texel column: adds up the slices front to back and writes each into its tile of
// the game's fog texture, as the game's fog pass writes it. In the physical look the last slice
// holds the height fog alone, which the sky and the clouds read: the sky's light already holds the
// air in front of it.
inline constexpr char FogIntegrate[] = PHANTOM_FX_FOG_INPUTS R"(
Texture3D<float4> Media : register(t0);
Texture3D<float4> FogOnly : register(t1);
RWTexture2D<float4> GameFog : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= TileSize)) return;

    float normalizeFactor = FogParam[1].y;
    if (!Physical)
    {
        float3 selfLuminance = FogMedium[7].xyz;
        float3 color = 0;
        for (uint slice = 0; slice < Slices; ++slice)
        {
            float4 media = Media[uint3(id.xy, slice)];
            color += media.rgb;
            uint2 tile = uint2(slice % 32, slice / 32);
            GameFog[tile * TileSize + id.xy] = saturate(float4((color + (1 - media.a) * selfLuminance) / normalizeFactor, media.a));
        }
        return;
    }

    float3 color = 0, fogColor = 0;
    float transmittance = 1, fogTransmittance = 1;
    for (uint slice = 0; slice < Slices; ++slice)
    {
        float4 media = Media[uint3(id.xy, slice)];
        float4 fogOnly = FogOnly[uint3(id.xy, slice)];
        color += transmittance * media.rgb;
        transmittance *= media.a;
        fogColor += fogTransmittance * fogOnly.rgb;
        fogTransmittance *= fogOnly.a;
        uint2 tile = uint2(slice % 32, slice / 32);
        float4 written = slice < Slices - 1 ? float4(color, transmittance) : float4(fogColor, fogTransmittance);
        GameFog[tile * TileSize + id.xy] = float4(max(written.rgb, 0) / normalizeFactor, saturate(written.a));
    }
}
)";

// The physical look's sky, in place of the game's sky draws (Sky_Draw_TppBaked, Sky_Draw_Tpp,
// Sky_Draw_TppNoSky), with their vertex shaders and bindings: the sky's light from the sky-view
// table and the game's cloud deck, the game's sun or moon sprite (MOON 1), the height fog in front of
// it from the fog's last slice, and the game's TppTonemap and encoding, or the encoding alone with HDR
// (HDR 1), which takes TppTonemap out of the scene. Without the game's dither, as its own sky draws.
inline constexpr char SkyDraw[] = "#define LINEAR_CLAMP_SAMPLER s11\n" PHANTOM_FX_ATMOSPHERE R"(
cbuffer cPSSystem : register(b0)
{
    float4 m_param, m_renderInfo, m_renderBuffer, m_dominantLightDir;
};

cbuffer cPSScene : register(b2)
{
    float4 m_scene[26]; // [25].y the fog's normalising factor
};

cbuffer cPSMaterial : register(b4)
{
    float4 m_materials[8]; // [1].xyz the TppTonemap curve
};

SamplerState MoonSampler : register(s0);
Texture2D<float4> MoonTexture : register(t0);
Texture2D<float4> GameFog : register(t12);

float3 TppTonemap(float3 color, float3 curve)
{
    float3 above = curve.y + curve.z - 1 / ((color + curve.z - curve.y) * curve.x);
    return color <= curve.y ? color : above;
}

float3 Encode(float3 linearColor)
{
    float3 high = 1.055 * pow(max(linearColor, 0.00001), 1 / 2.4) - 0.055;
    return linearColor <= 0.0031308 ? linearColor * 12.92 : high;
}

float4 main(float4 position : SV_Position, float3 ray : TEXCOORD0, float4 moonPlace : TEXCOORD1, float4 moonColor : TEXCOORD2) : SV_Target
{
    float4 sun = SkyParameters.Load(int3(SunTexel, 0, 0));
    float3 towardsSun = SkyParameters.Load(int3(DirectionTexel, 0, 0)).xyz;
    float4 deck = SkyParameters.Load(int3(OvercastTexel, 0, 0));
    // The sun's share in sun.rgb is what the deck leaves of the sky.
    float3 color = SkyLight(normalize(ray), towardsSun, sun.a) * sun.rgb + deck.rgb;

#if MOON
    float4 moon = moonColor * MoonTexture.Sample(MoonSampler, moonPlace.zw);
    moon.a *= 1 - deck.a;
    if (all(moonPlace.zw >= 0 && moonPlace.zw <= 1)) color = color * (1 - moon.a) + saturate(moon.a * moon.rgb);
#endif

    float2 pixel = position.xy - 0.5;
    float2 clip = pixel / m_renderInfo.xy * float2(2, -2) + float2(-1, 1);
    float4 fog = GameFog.SampleLevel(LinearClamp, clip * float2(0.014648, 0.123047) + float2(0.015625, 0.125) + float2(0.96875, 0.75), 0);
    color = fog.rgb * m_scene[25].y + color * fog.a;

#if HDR
    // Clamped to 1 as the game's sky draws stay with HDR (hdr.cpp): above it the sun's sprite shows as
    // a ring around a dimmer centre instead of a disc. One unit is paper white.
    return float4(Encode(min(max(color, 0), 1)), 0);
#else
    return float4(Encode(saturate(TppTonemap(color, m_materials[1].xyz))), 0);
#endif
}
)";
}
