// Path-traced lamps (docs/DEVELOPMENT.md, "Path tracing"): with DLSS Ray Reconstruction denoising, every
// render pixel's light from the frame's point and spot lights, the flashlights among them, in place of the
// game's light shaders. Each light's light as those shaders work it out (its falloff, a spot's cone and
// mask, the game's volume for it, its diffuse and specular scales, the foliage's light through its leaves),
// with GGX's specular as the traced reflections and the sun have it.
//
// The light a pixel takes is picked by ReSTIR DI (Bitterli et al., "Spatiotemporal reservoir resampling for
// real-time ray tracing with dynamic direct lighting", SIGGRAPH 2020), weighed by its light there without
// shadows (the target):
// - SampleLamps: a few lights at random (every light when there are no more), a reservoir of one of them
//   (resampled importance sampling), a shadow ray to it, and the pixel's reservoir of the last frame, where
//   it was, joined to it;
// - ShadeLamps: the reservoirs of a few neighbours on the same surface joined to the pixel's, and a shadow
//   ray to the light picked, whose light times its contribution weight goes out; the reservoir stays for the
//   next frame.
// The joins weigh each reservoir by the samples it stands for (the biased 1/M weights of Bitterli's paper),
// with a neighbour or the last frame taken only on a surface alike in depth and normal. The light goes out
// times the exposure, for the game's light targets; Ray Reconstruction denoises it.
#include "gi_shadow.hlsli"
#include "ggx_specular.hlsli"

Texture2D<float4> g_Guide_NormalRoughness : register(t18); // The world-space normal, the roughness in w (ray_tracing::ReconstructionGuides)
Texture2D<float4> g_LampMasks : register(t20);             // The masks: GI_LAMP_MASK_TILES tiles across
Texture2D<float4> g_Guide_Material : register(t21);        // rgb: the specular colour at normal incidence; a: the translucency
Texture2D<float> g_Guide_Translucency : register(t22);     // The light through the leaves' floor: the translucency times the material's
RWTexture2D<float4> g_LampDiffuse : register(u95);         // The lights' diffuse light, times the exposure
RWTexture2D<float4> g_LampSpecular : register(u96);        // Their specular light
RWTexture2D<uint4> g_LampReservoirs : register(u97);       // This frame's reservoirs, SampleLamps' (PackReservoir)
RWTexture2D<uint4> g_LampHistory : register(u98);          // ShadeLamps' reservoirs, the last frame's for SampleLamps

// Lights a pixel picks from at random, and neighbours it joins; how far apart those are, in pixels.
static const uint Candidates = 8;
static const uint Neighbours = 4;
static const float NeighbourRadius = 24.0f;
// The most samples the last frame's reservoir stands for, against the frame's own one (Bitterli's 20).
static const float HistoryCap = 20.0f;
// A reservoir without a light.
static const uint NoLamp = 0xFFFFu;

// A masked spot's mask at `world`, as its shader projects and samples it (wrapped, bilinear); 1 for the others.
float3 MaskAt(GILocalLight light, float3 world)
{
    uint tile = light.maskTile.x;
    if (tile == 0u) return 1.0f;

    float4 position = float4(world, 1.0f);
    float3 projected = float3(dot(light.mask[0], position), dot(light.mask[1], position), dot(light.mask[2], position));
    if (projected.z <= 0.0f) return 0.0f;

    float2 uv = projected.xy / projected.z * 0.5f + 0.5f;
    uv = frac(float2(uv.x, 1.0f - uv.y));
    // Half a texel in from the tile's edges, so the bilinear taps stay in the tile.
    float inset = 0.5f / GI_LAMP_MASK_SIZE;
    uv = clamp(uv, inset, 1.0f - inset);
    return g_LampMasks.SampleLevel(g_LinearSampler, float2((float(tile - 1u) + uv.x) / GI_LAMP_MASK_TILES, uv.y), 0.0f).rgb;
}

// The surface a pixel's lights light.
struct LitSurface
{
    float3 world, normal, toEye, specularColor;
    float roughness, translucency, translucencyFloor, viewZ;
};

// Render pixel `pixel`'s surface; false for the sky.
bool SurfaceAt(uint2 pixel, out LitSurface surface)
{
    surface = (LitSurface)0;
    float depth = g_Depth.Load(int3(pixel, 0));
    if (depth <= 0.0f) return false;

    float4 normalRoughness = g_Guide_NormalRoughness.Load(int3(pixel, 0));
    float4 material = g_Guide_Material.Load(int3(pixel, 0));
    surface.world = DepthPosition(pixel, depth);
    surface.normal = normalize(normalRoughness.xyz);
    surface.toEye = normalize(g_Frame.eye.xyz - surface.world);
    surface.roughness = normalRoughness.w;
    surface.specularColor = material.rgb;
    surface.translucency = material.a;
    surface.translucencyFloor = g_Guide_Translucency.Load(int3(pixel, 0));
    surface.viewZ = toLinearDepth(depth, g_Frame.nearFar.xy);
    return true;
}

// Whether a neighbour's or the last frame's surface (its view depth and normal) is close enough to share
// its reservoir: within 10% in depth and 25 degrees in normal.
bool IsAlike(float viewZ, float3 normal, float otherViewZ, float3 otherNormal)
{
    return abs(otherViewZ - viewZ) < 0.1f * viewZ && dot(normal, otherNormal) > 0.9f;
}

// A light's diffuse and specular light at the surface without shadows; false where it gives none.
bool LampLightAt(GILocalLight light, LitSurface surface, out float3 diffuse, out float3 specular, out float3 toLight)
{
    float lightDistance;
    float3 reaching = LocalLightReaching(light, surface.world, toLight, lightDistance) * MaskAt(light, surface.world);
    diffuse = 0.0f;
    specular = 0.0f;
    if (all(reaching <= 0.0f)) return false;

    // The game's light through leaves (LN_trans): lit from behind as much as the translucency lets it.
    float LN = dot(surface.normal, toLight);
    float wrap = saturate(max(surface.translucencyFloor, surface.translucency * (1.0f - LN * LN) + LN));
    diffuse = reaching * (wrap * light.scales.y);
    specular = GgxSpecular(reaching, surface.normal, surface.toEye, toLight, surface.roughness, surface.specularColor) * light.scales.x;
    return any(diffuse > 0.0f) || any(specular > 0.0f);
}

// The target a light is picked by: its light's luminance at the surface without shadows.
float TargetOf(uint light, LitSurface surface)
{
    float3 diffuse, specular, toLight;
    if (!LampLightAt(g_Frame.localLights[light], surface, diffuse, specular, toLight)) return 0.0f;

    return luminance(diffuse + specular);
}

// How much of a light reaches the surface: a shadow ray to a point on its disc facing the pixel (its radius in
// falloff.w), leaving as the ray-traced lights' rays leave (hair's share, the faces that cast).
float VisibilityOf(GILocalLight light, LitSurface surface, ShadowSurface shadowSurface, float2 cone, inout Random random)
{
    float3 toLight = normalize(light.position.xyz - surface.world);
    float3 target = light.position.xyz + DiscOffset(toLight, light.falloff.w, random.rand2());
    float3 offset = target - surface.world;
    float targetDistance = length(offset);
    float3 direction = offset / targetDistance;
    float3 origin, facing;
    float tolerance;
    ShadowRayStart(shadowSurface, direction, origin, facing, tolerance);
    float rayLength = targetDistance - ClearanceTowards(light, direction);
    if (rayLength <= 0.0f) return 1.0f;

    float blocker = FLT_MAX, transmittance = 1.0f;
    bool isBlocked = SurfaceRayBlocked(origin, facing, tolerance, direction, rayLength, cone, true, 0.0f, ShadowRayTowards(RayTowardsLamp), random, blocker,
        transmittance);
    return isBlocked ? 0.0f : transmittance;
}

// A pixel's reservoir: its light, the light's contribution weight (W), the samples it stands for (M), and the
// surface it was made for, which the next frame's surface must be alike to take it.
struct LampReservoir
{
    uint light;
    float weight, samples;
    float3 normal;
    float viewZ;
};

LampReservoir EmptyReservoir()
{
    LampReservoir reservoir = (LampReservoir)0;
    reservoir.light = NoLamp;
    return reservoir;
}

uint4 PackReservoir(LampReservoir reservoir)
{
    return uint4(reservoir.light | (f32tof16(reservoir.samples) << 16), asuint(reservoir.weight), packNormal(reservoir.normal), asuint(reservoir.viewZ));
}

LampReservoir UnpackReservoir(uint4 packed)
{
    LampReservoir reservoir;
    reservoir.light = packed.x & 0xFFFFu;
    reservoir.samples = f16tof32(packed.x >> 16);
    reservoir.weight = asfloat(packed.y);
    reservoir.normal = unpackNormal(packed.z);
    reservoir.viewZ = asfloat(packed.w);
    return reservoir;
}

// Reservoirs joined into one as they stream in (weighted reservoir sampling): the light picked, its target,
// the weights' sum and the samples they stand for.
struct Join
{
    uint light;
    float target, weightSum, samples;
};

Join EmptyJoin()
{
    Join join;
    join.light = NoLamp;
    join.target = join.weightSum = join.samples = 0.0f;
    return join;
}

void Take(inout Join join, uint light, float target, float weight, float samples, inout Random random)
{
    join.weightSum += weight;
    join.samples += samples;
    if (weight > 0.0f && random.rand() * join.weightSum < weight)
    {
        join.light = light;
        join.target = target;
    }
}

// A reservoir of `surface` joined into `join`, its light's target taken at `surface`.
void TakeReservoir(inout Join join, LampReservoir reservoir, LitSurface surface, inout Random random)
{
    float target = reservoir.light != NoLamp ? TargetOf(reservoir.light, surface) : 0.0f;
    Take(join, reservoir.light, target, target * reservoir.weight * reservoir.samples, reservoir.samples, random);
}

// The joined reservoir for `surface`: its light's contribution weight from the weights' sum over its target
// and the samples (Bitterli's biased 1/M).
LampReservoir ReservoirOf(Join join, LitSurface surface)
{
    LampReservoir reservoir;
    reservoir.light = join.target > 0.0f ? join.light : NoLamp;
    reservoir.weight = join.target > 0.0f ? join.weightSum / (join.target * join.samples) : 0.0f;
    reservoir.samples = join.samples;
    reservoir.normal = surface.normal;
    reservoir.viewZ = surface.viewZ;
    return reservoir;
}

// The light the last frame's light `light` is in this frame's list (ReSTIR's light history, gi_renderer.cpp);
// NoLamp for one that went.
uint CurrentLampOf(uint light)
{
    if (light >= GI_MAX_LOCAL_LIGHTS) return NoLamp;

    uint current = g_Frame.lampHistory[light / 4u][light % 4u];
    return current < g_Frame.localLightCount.x ? current : NoLamp;
}

// Where the pixel was last frame on screen, and its view depth then: the camera's motion, or a moving mesh's
// from the game's velocity, its depth then taken as its own now (PrepareGuides).
float2 LastFrameUv(uint2 pixel, LitSurface surface, out float lastViewZ)
{
    float2 motion;
    CameraMotionOf(pixel, g_Depth.Load(int3(pixel, 0)), motion, lastViewZ);
    float2 moved;
    if (ObjectMotion(int2(pixel), moved))
    {
        motion = JitterUv(g_Frame.jitter.xy) - moved / g_Frame.dimensions.xy;
        lastViewZ = surface.viewZ;
    }

    return (pixel + 0.5f) / g_Frame.dimensions.xy + motion;
}

[numthreads(8, 8, 1)]
void SampleLamps(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    LitSurface surface;
    uint count = g_Frame.localLightCount.x;
    if (!SurfaceAt(did, surface) || count == 0u)
    {
        g_LampReservoirs[did] = PackReservoir(EmptyReservoir());
        return;
    }

    // Apart from the global illumination's and the reflections' rays from the same pixel.
    uint pixels = g_Frame.dimensions.x * g_Frame.dimensions.y;
    Random random = MakeRandom(did.x + did.y * g_Frame.dimensions.x + 2u * pixels, g_Frame.dimensions.z);

    // The candidates: every light when there are no more, else a few uniformly, each weighed by its target over
    // its chance.
    bool isEveryLight = count <= Candidates;
    uint candidates = isEveryLight ? count : Candidates;
    float candidateScale = isEveryLight ? 1.0f : float(count);
    Join initial = EmptyJoin();
    for (uint i = 0; i < candidates; ++i)
    {
        uint light = isEveryLight ? i : min(uint(random.rand() * count), count - 1u);
        float target = TargetOf(light, surface);
        Take(initial, light, target, target * candidateScale, 1.0f, random);
    }

    // One sample, whichever way it was picked; none where its shadow ray is blocked (visibility reuse).
    initial.samples = isEveryLight ? 1.0f : float(candidates);
    LampReservoir picked = ReservoirOf(initial, surface);
    picked.samples = 1.0f;
    if (picked.light != NoLamp)
    {
        ShadowSurface shadowSurface = ShadowSurfaceAt(surface.world, DecodeNormal(g_Normal.Load(int3(did, 0))));
        picked.weight *= VisibilityOf(g_Frame.localLights[picked.light], surface, shadowSurface, PixelCone(surface.world, 0.0f), random);
    }

    Join join = EmptyJoin();
    TakeReservoir(join, picked, surface, random);

    // The last frame's, where the pixel was, on a surface alike, its light as it is this frame.
    float lastViewZ;
    float2 lastUv = LastFrameUv(did, surface, lastViewZ);
    if (g_Frame.localLightCount.z != 0u && all(lastUv > 0.0f) && all(lastUv < 1.0f))
    {
        LampReservoir last = UnpackReservoir(g_LampHistory[uint2(lastUv * g_Frame.dimensions.xy)]);
        last.light = last.light != NoLamp ? CurrentLampOf(last.light) : NoLamp;
        last.samples = min(last.samples, HistoryCap);
        if (last.light != NoLamp && IsAlike(lastViewZ, surface.normal, last.viewZ, last.normal)) TakeReservoir(join, last, surface, random);
    }

    g_LampReservoirs[did] = PackReservoir(ReservoirOf(join, surface));
}

[numthreads(8, 8, 1)]
void ShadeLamps(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    LitSurface surface;
    if (!SurfaceAt(did, surface) || g_Frame.localLightCount.x == 0u)
    {
        g_LampDiffuse[did] = 0.0f;
        g_LampSpecular[did] = 0.0f;
        g_LampHistory[did] = PackReservoir(EmptyReservoir());
        return;
    }

    uint pixels = g_Frame.dimensions.x * g_Frame.dimensions.y;
    Random random = MakeRandom(did.x + did.y * g_Frame.dimensions.x + 3u * pixels, g_Frame.dimensions.z);

    // The pixel's own, then its neighbours' on a surface alike, each light's target taken here.
    Join join = EmptyJoin();
    TakeReservoir(join, UnpackReservoir(g_LampReservoirs[did]), surface, random);
    for (uint i = 0; i < Neighbours; ++i)
    {
        float2 u = random.rand2();
        float2 offset = NeighbourRadius * sqrt(u.x) * float2(cos(TWO_PI * u.y), sin(TWO_PI * u.y));
        int2 neighbour = int2(did) + int2(round(offset));
        if (any(neighbour < 0) || any(neighbour >= int2(g_Frame.dimensions.xy)) || all(neighbour == int2(did))) continue;

        LampReservoir other = UnpackReservoir(g_LampReservoirs[uint2(neighbour)]);
        if (other.light != NoLamp && IsAlike(surface.viewZ, surface.normal, other.viewZ, other.normal)) TakeReservoir(join, other, surface, random);
    }

    LampReservoir reservoir = ReservoirOf(join, surface);
    float3 diffuseLight = 0.0f, specularLight = 0.0f;
    if (reservoir.light != NoLamp)
    {
        GILocalLight light = g_Frame.localLights[reservoir.light];
        float3 diffuse, specular, toLight;
        LampLightAt(light, surface, diffuse, specular, toLight);
        ShadowSurface shadowSurface = ShadowSurfaceAt(surface.world, DecodeNormal(g_Normal.Load(int3(did, 0))));
        float visibility = VisibilityOf(light, surface, shadowSurface, PixelCone(surface.world, 0.0f), random);
        diffuseLight = diffuse * (visibility * reservoir.weight);
        specularLight = specular * (visibility * reservoir.weight);
        // A light its ray found blocked goes on with nothing, so the next frames do not spread it.
        reservoir.weight *= visibility;
    }

    g_LampDiffuse[did] = float4(RemoveNaNs(diffuseLight) * g_Frame.nearFar.w, 0.0f);
    g_LampSpecular[did] = float4(RemoveNaNs(specularLight) * g_Frame.nearFar.w, 0.0f);
    g_LampHistory[did] = PackReservoir(reservoir);
}
