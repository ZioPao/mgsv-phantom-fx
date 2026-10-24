// The sun's ray-traced shadows (docs/DEVELOPMENT.md, "Ray-traced shadows"): a shadow ray from each
// pixel towards a point on the sun's disc, then the temporal pass of AMD's FidelityFX Shadow
// Denoiser (gi_shadow.hlsli); gi_shadow_filter.hlsl has its spatial passes. Also the point and spot
// lights' shadows (docs/DEVELOPMENT.md, "Ray-traced lights"), a bit a light.
//
// Cut-out geometry with the game's alpha (GI_GEOMETRY_ALPHA_TEXTURE) casts its own shadow, holes and
// all. The game's shadow map keeps the shadows of cut-out geometry without it where it reaches: the
// rays let the sun through it there, and the sun pass takes the darker of the two shadows. Beyond the
// shadow map, where the game has no shadow, the rays take it as letting half the sun through.
//
// Where it deviates from the denoiser's sample, and why:
// - The trace writes the denoiser's 8x4 tile masks itself, through groupshared memory, which works
//   at any wave size.
// - Motion comes from the camera, and from the game's velocity where a moving mesh drew
//   (gi_shadow.hlsli, ReadVelocity).
#include "gi_shadow.hlsli"
// The denoiser as AMD wrote it; its bitfield helpers shift an untyped literal.
#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#include "ffx_denoiser_shadows/ffx_denoiser_shadows_tileclassification.h"
#pragma dxc diagnostic pop

// The sun's direction, spread over its disc: a uniform point on a disc of the angular radius's
// tangent around it.
float3 SunSample(float2 samples)
{
    float3 toSun = g_Frame.sunDirection.xyz;
    float3 u, v;
    GetOrthoVectors(toSun, u, v);

    float radius = g_Frame.shadowParams.x * sqrt(samples.x);
    float angle = TWO_PI * samples.y;
    return normalize(toSun + radius * (cos(angle) * u + sin(angle) * v));
}

// How far a point lies beyond the game's shadow map: 0 inside it, 1 past its fade, as
// SunLight_DL_CS_ES fades its shadow to lit (shadows_shaders.hpp, SunShadow). In view space the
// fade runs from the point (0, 0, c8.x); its distance from there follows from the distance to the
// eye and the view depth.
float BeyondShadowMap(float3 position, float depth)
{
    if (g_Frame.shadowRange.w == 0.0f) return 1.0f;

    float centre = g_Frame.shadowRange.x;
    float viewDepth = toLinearDepth(depth, g_Frame.nearFar.xy);
    float toEye = distance(position, g_Frame.eye.xyz);
    float fromCentre = sqrt(max(toEye * toEye - 2.0f * viewDepth * centre + centre * centre, 0.0f));
    float fade = saturate(max(fromCentre - g_Frame.shadowRange.y, 0.0f) * g_Frame.shadowRange.z) * (viewDepth >= centre ? 1.0f : 0.0f);
    return fade * fade;
}

// ---- Where a pixel's shadow rays leave its surface ----
// The scene's own surface where a ray from the eye through the pixel meets it near the depth's point: its
// exact point and its triangle's normal, from which a ray leaves past that very surface (LiftOffTriangle),
// so nothing near it needs skipping.
// Where the game drew something the scene lacks or holds at another level of detail, the depth's point
// and the G-buffer's normal, with copies of the surface skipped (gi_common.hlsli). Until 2026-10-24
// every ray left that way: the skip took a character pressed to a wall for a copy of the wall, and
// light went through him, ragged along the wall's bumpy normals.
struct ShadowSurface
{
    float3 position;
    float3 normal; // Towards the eye: the triangle's on the scene's surface, else the G-buffer's
    bool exact;    // On the scene's own surface
    SceneHit hit;  // Diagnostics (the probe): the eye's ray's hit on it
};

ShadowSurface ShadowSurfaceAt(float3 position, float3 gbufferNormal)
{
    ShadowSurface surface;
    surface.position = position;
    surface.normal = gbufferNormal;
    surface.exact = false;
    surface.hit = (SceneHit)0;
    // Diagnostics: every ray from the depth, as before.
    if (g_Frame.shadowDiagnostics.x) return surface;

    // Only the stretch around the depth's point: the game hides what comes between the camera and the player
    // (a bush the camera backs into) and the scene keeps it. Searched from the eye, the ray stopped on its
    // leaves and the pixels behind them left from the depth's point, inside the coarser rock behind for some:
    // the bush's leaves showed as shadows on what lay behind (the shadow probe, 2026-10-24: the camera's ray
    // met leaves 0.1 to 1.4 m away, the game's surface 2.4 to 5.9 m).
    float3 toSurface = position - g_Frame.eye.xyz;
    float surfaceDistance = length(toSurface);
    float3 direction = toSurface / max(surfaceDistance, 1e-6f);
    float tolerance = 0.03f + 0.02f * surfaceDistance;
    SceneHit hit = TraceClosest(g_Frame.eye.xyz, direction, max(surfaceDistance - tolerance, 0.0f), surfaceDistance + tolerance, GI_MASK_SCENE, true,
        false, float2(0.0f, g_Frame.rayCone.x));
    if (!hit.hit || abs(hit.distance - surfaceDistance) > tolerance) return surface;

    SceneTriangle t = LoadTriangle(hit.instance, hit.geometry, hit.primitive);
    float3 normal = cross(t.v1 - t.v0, t.v2 - t.v0);
    float lengthSquared = dot(normal, normal);
    if (!(lengthSquared > 0.0f)) return surface;

    normal *= rsqrt(lengthSquared);
    surface.normal = dot(normal, direction) > 0.0f ? -normal : normal;
    surface.position = (1.0f - hit.barycentrics.x - hit.barycentrics.y) * t.v0 + hit.barycentrics.x * t.v1 + hit.barycentrics.y * t.v2;
    surface.exact = true;
    surface.hit = hit;
    return surface;
}

// A shadow ray's start off a point on the scene's own surface: 16 float steps of the point's largest
// coordinate along the normal, about 2 mm at 1,200 m from the map's origin, where the game's maps reach.
// The point is worked out from the triangle's corners, and the ray is traced in the instance's own space,
// each a few steps of the largest coordinate off in any direction: a character's transform turns the
// error of a large coordinate into the others. Wächter and Binder's offset (OffsetFromTriangle, 256 steps
// of each coordinate on its own) lifted the start about 3 cm in the field, more than the folds of a
// character's clothes, so it sat inside the next fold and his chest was shadowed in strips; and next to
// nothing in the helicopter, whose floor lies near the map's origin across but 1,200 m up, so a face
// turned sideways hit its own triangle in every other frame (the shadow probe, 2026-10-24). Coordinates
// under 128 m count as 128: a large model's own coordinates reach that far.
float3 LiftOffTriangle(float3 position, float3 normal)
{
    const float steps = 16.0f;
    const float step = 1.0f / 8388608.0f; // A float step, relative

    float largest = max(max(abs(position.x), abs(position.y)), max(abs(position.z), 128.0f));
    return position + normal * (steps * step * largest);
}

// A shadow ray's start towards `toLight`: from the surface's side facing the light, as leaves lit from
// behind take light through them.
void ShadowRayStart(ShadowSurface surface, float3 toLight, out float3 origin, out float3 facing, out float tolerance)
{
    facing = dot(surface.normal, toLight) >= 0.0f ? surface.normal : -surface.normal;
    origin = surface.exact ? LiftOffTriangle(surface.position, facing) : OffsetFromScreenSurface(surface.position, facing);
    tolerance = surface.exact ? 0.0f : CopyTolerance(surface.position);
}

// Whether a ray leaving a surface reaches a light `rayLength` away. Near the surface, copies of it are
// skipped (gi_common.hlsli, "Copies of a surface"); further on, cut-out geometry with the game's alpha
// blocks it where its alpha does, and cut-out geometry without its texture with chance `blocking`, so
// the denoiser averages a partial shadow.
// How far along the ray it is stopped: FLT_MAX where it gets through.
float ShadowRayBlocker(float3 origin, float3 normal, float tolerance, float3 direction, float rayLength, float blocking, float2 cone, inout Random random)
{
    float nearEnd = min(tolerance / max(dot(direction, normal), 0.1f), rayLength);
    SceneHit nearHit = TraceFromSurface(origin, normal, tolerance, direction, nearEnd, GI_MASK_SCENE, true, cone);
    if (nearHit.hit) return nearHit.distance;
    if (nearEnd >= rayLength) return FLT_MAX;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = nearEnd;
    ray.TMax = rayLength;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;
    query.TraceRayInline(g_Scene, GI_RAY_FLAGS, GI_MASK_SCENE, ray);
    // Only cut-out geometry is non-opaque, so only it comes here.
    while (query.Proceed())
    {
        uint row = query.CandidateInstanceContributionToHitGroupIndex() + query.CandidateGeometryIndex();
        float footprint = cone.x + query.CandidateTriangleRayT() * cone.y;
        if (CutOutBlocks(row, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), random.rand() < blocking, footprint,
                query.CandidateObjectToWorld3x4()))
            query.CommitNonOpaqueTriangleHit();
    }

    return query.CommittedStatus() == COMMITTED_NOTHING ? FLT_MAX : query.CommittedRayT();
}

bool ShadowRayVisible(float3 origin, float3 normal, float tolerance, float3 direction, float rayLength, float blocking, float2 cone, inout Random random)
{
    return ShadowRayBlocker(origin, normal, tolerance, direction, rayLength, blocking, cone, random) == FLT_MAX;
}

groupshared uint g_TileMask;

// One 8x4 tile a group: a ray a pixel, into the tile's mask. The sky and pixels past the edge count
// as lit, so they do not keep the denoiser from skipping a lit tile.
[numthreads(8, 4, 1)]
void TraceSunShadows(uint2 gtid : SV_GroupThreadID, uint2 gid : SV_GroupID, uint index : SV_GroupIndex)
{
    if (index == 0) g_TileMask = 0;
    GroupMemoryBarrierWithGroupSync();

    uint2 did = gid * uint2(8, 4) + gtid;
    bool lit = true;
    if (all(did < g_Frame.dimensions.xy))
    {
        float depth = g_Depth.Load(int3(did, 0));
        if (depth > 0.0f)
        {
            float2 uv = (did + 0.5f) / g_Frame.dimensions.xy;
            float3 position = transformPointProjection(uv, depth, g_Frame.viewProjectionInverse);
            float3 normal = DecodeNormal(g_Normal.Load(int3(did, 0)));
            Random random = MakeRandom(g_Frame.dimensions.z, did.y * g_Frame.dimensions.x + did.x);
            float3 toSun = SunSample(random.rand2());
            float3 origin, facing;
            float tolerance;
            ShadowRayStart(ShadowSurfaceAt(position, normal), toSun, origin, facing, tolerance);
            float blocking = BeyondShadowMap(position, depth) * g_Frame.shadowParams.y;
            // Rays to the sun run parallel: each keeps its pixel's footprint. The sun's size comes from
            // spreading the rays over its disc; a cone widened by it as well took cut-out alpha from
            // coarse levels of detail, where a pylon's thin bars fell under the threshold.
            lit = ShadowRayVisible(origin, facing, tolerance, toSun, g_Frame.shadowParams.z, blocking, PixelCone(position, 0.0f), random);
        }
    }

    if (lit) InterlockedOr(g_TileMask, FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(did));
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) g_Shadow_Masks[FFX_DNSR_Shadows_LinearTileIndex(gid, g_Frame.dimensions.x)] = g_TileMask;
}

// ---- Point and spot lights ----
// Soft shadows for all the frame's point and spot lights in one value a pixel: the share of their
// light that reaches it, the ratio estimator of Heitz, Hill and McGuire ("Combining Analytic Direct
// Illumination and Stochastic Shadows", I3D 2018) over the lights together. The game's light shaders
// give each light's light; rays give the share: g_Frame.localLightCount.y rays a pixel, each to a point
// on a light chosen in proportion to its light there without shadows (systematic sampling, so the rays
// share out over the lights), light that gets through over all the light. Every light's light times
// the share gives each light its own shadow where lights do not overlap, and the right diffuse light
// in total where they do. A spatial filter no wider than the penumbra (FilterLocalLightShadows) smooths
// the rays' noise and keeps contact shadows sharp; no history, so nothing trails.

RWTexture2D<float> g_LocalLightShadow : register(u89);   // What the light pass reads: the filtered share
RWTexture2D<float2> g_LocalShadowRays : register(u90);   // x: the share the rays found; y: the penumbra's width, pixels; then the second pass's
RWTexture2D<float2> g_LocalShadowFirst : register(u91);  // The first pass's

// The penumbra the filter may blur across, pixels: at most this wide.
static const float MaxPenumbraPixels = 64.0f;

[numthreads(8, 8, 1)]
void TraceLocalLightShadows(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    float2 result = float2(1.0f, 0.0f);
    float depth = g_Depth.Load(int3(did, 0));
    if (depth > 0.0f)
    {
        float2 uv = (did + 0.5f) / g_Frame.dimensions.xy;
        float3 position = transformPointProjection(uv, depth, g_Frame.viewProjectionInverse);
        float3 normal = DecodeNormal(g_Normal.Load(int3(did, 0)));
        float3 toLight;
        float lightDistance;
        float total = 0.0f;
        for (uint i = 0; i < g_Frame.localLightCount.x; ++i) total += luminance(LocalLightAt(g_Frame.localLights[i], position, normal, toLight, lightDistance));

        if (total > 0.0f)
        {
            Random random = MakeRandom(g_Frame.dimensions.z, did.y * g_Frame.dimensions.x + did.x);
            ShadowSurface surface = ShadowSurfaceAt(position, normal);
            float2 cone = PixelCone(position, 0.0f);
            uint rays = max(g_Frame.localLightCount.y, 1u);
            float spacing = total / float(rays);
            float threshold = random.rand() * spacing;
            float running = 0.0f, penumbra = 0.0f;
            uint traced = 0, visible = 0, blocked = 0;
            for (uint i = 0; i < g_Frame.localLightCount.x && traced < rays; ++i)
            {
                GILocalLight light = g_Frame.localLights[i];
                running += luminance(LocalLightAt(light, position, normal, toLight, lightDistance));
                while (traced < rays && threshold < running)
                {
                    // A point on the light's disc facing the pixel, its radius in falloff.w.
                    float3 b1, b2;
                    GetOrthoVectors(toLight, b1, b2);
                    float2 disc = random.rand2();
                    float radius = light.falloff.w * sqrt(disc.x);
                    float angle = TWO_PI * disc.y;
                    float3 target = light.position.xyz + radius * (cos(angle) * b1 + sin(angle) * b2);
                    float3 offset = target - position;
                    float targetDistance = length(offset);
                    float3 direction = offset / targetDistance;

                    float3 origin, facing;
                    float tolerance;
                    ShadowRayStart(surface, direction, origin, facing, tolerance);
                    float rayLength = targetDistance - light.shape.x;
                    float blocker = rayLength > 0.0f ? ShadowRayBlocker(origin, facing, tolerance, direction, rayLength, 0.0f, cone, random) : FLT_MAX;
                    if (blocker == FLT_MAX) ++visible;
                    else
                    {
                        // The penumbra a blocker this far along casts from a light this wide, metres.
                        penumbra += 2.0f * light.falloff.w * blocker / max(targetDistance - blocker, 1e-3f);
                        ++blocked;
                    }

                    ++traced;
                    threshold += spacing;
                }
            }

            float footprint = max(distance(position, g_Frame.eye.xyz) * g_Frame.rayCone.x, 1e-6f);
            result = float2(float(visible) / float(max(traced, 1u)), blocked ? min(penumbra / float(blocked) / footprint, MaxPenumbraPixels) : 0.0f);
        }
    }

    g_LocalShadowRays[did] = result;
}

// Three edge-avoiding a-trous passes (Dammertz et al., "Edge-Avoiding A-Trous Wavelet Transform for
// Fast Global Illumination Filtering", 2010), 1, 2 and 4 pixels apart: a neighbour counts by a 3 x 3
// B-spline's weight, as near as its depth and normal are. A pixel whose penumbra is narrower than the
// pass's step keeps its value, so a contact shadow stays as sharp as the rays found it.
// g_Dispatch: x the pass (0 to 2), y its step in pixels. The first reads the rays' result and writes
// g_LocalShadowFirst, the second writes back into g_LocalShadowRays, the third what the light pass reads.
[numthreads(8, 8, 1)]
void FilterLocalLightShadows(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    uint pass = g_Dispatch.x;
    int step = int(g_Dispatch.y);
    float2 centre = pass == 1 ? g_LocalShadowFirst[did] : g_LocalShadowRays[did];
    float2 filtered = centre;
    float depth = g_Depth.Load(int3(did, 0));
    if (depth > 0.0f && centre.y >= float(step))
    {
        float centreDepth = toLinearDepth(depth, g_Frame.nearFar.xy);
        float3 centreNormal = DecodeNormal(g_Normal.Load(int3(did, 0)));
        float2 sum = 0.0f;
        float weights = 0.0f;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int2 pixel = clamp(int2(did) + int2(x, y) * step, 0, int2(g_Frame.dimensions.xy) - 1);
                float neighbourDepth = g_Depth.Load(int3(pixel, 0));
                if (neighbourDepth <= 0.0f) continue;

                float2 value = pass == 1 ? g_LocalShadowFirst[pixel] : g_LocalShadowRays[pixel];
                float weight = (x == 0 ? 0.5f : 0.25f) * (y == 0 ? 0.5f : 0.25f);
                weight *= exp(-abs(toLinearDepth(neighbourDepth, g_Frame.nearFar.xy) - centreDepth) / (0.02f * centreDepth + 0.01f));
                weight *= pow(saturate(dot(centreNormal, DecodeNormal(g_Normal.Load(int3(pixel, 0))))), 8.0f);
                sum += weight * value;
                weights += weight;
            }

        if (weights > 0.0f) filtered = sum / weights;
    }

    if (pass == 2) g_LocalLightShadow[did] = filtered.x;
    else if (pass == 0) g_LocalShadowFirst[did] = filtered;
    else g_LocalShadowRays[did] = filtered;
}

// The denoiser's temporal pass, an 8x8 tile a group. It also keeps this frame's depth for the next.
[numthreads(64, 1, 1)]
void ClassifyShadowTiles(uint index : SV_GroupIndex, uint2 gid : SV_GroupID)
{
    uint2 did = gid * 8 + FFX_DNSR_Shadows_RemapLane8x8(index);
    if (all(did < g_Frame.dimensions.xy)) g_Shadow_Depths[0][did] = g_Depth.Load(int3(did, 0));

    FFX_DNSR_Shadows_TileClassification(index, gid);
}

// ---- Diagnostics: the shadow debug view and probe ----
// The sun ray from a pixel as the shadow rays trace it, but towards the sun's centre and without
// chance; and the whole ray past copies of the surface, cut-outs without their texture letting it through.

RWStructuredBuffer<GIShadowProbe> g_ShadowProbe : register(u77);
RWTexture2D<float4> g_ShadowDebug : register(u78); // The debug view, blended over the image

struct DebugRay
{
    ShadowSurface surface; // The surface it leaves
    float3 origin, normal, direction;
    float tolerance, nearEnd, beyond;
    uint copies;      // Copies of the surface skipped in the near segment
    SceneHit nearHit; // The shadow rays' near segment, past copies
    SceneHit farHit;  // The shadow rays' far segment; beyond the shadow map, cut-out geometry without the game's alpha as opaque
    SceneHit anyHit;  // The whole ray, past copies near the surface; cut-outs without their texture let it through
};

DebugRay TraceDebugRay(float3 position, float3 normal, float depth)
{
    DebugRay r;
    r.direction = g_Frame.sunDirection.xyz;
    r.surface = ShadowSurfaceAt(position, normal);
    ShadowRayStart(r.surface, r.direction, r.origin, r.normal, r.tolerance);
    r.beyond = BeyondShadowMap(position, depth);
    float rayLength = g_Frame.shadowParams.z;
    r.nearEnd = min(r.tolerance / max(dot(r.direction, r.normal), 0.1f), rayLength);

    // As TraceFromSurface, counting the copies it skips.
    r.copies = 0;
    float start = 0.0f;
    for (uint attempt = 0; attempt < 4; ++attempt)
    {
        r.nearHit = TraceClosest(r.origin, r.direction, start, r.nearEnd, GI_MASK_SCENE, true, false, PixelCone(position, 0.0f));
        if (!r.nearHit.hit || !IsCopyOfSurface(r.nearHit, r.normal, r.direction, r.tolerance)) break;

        ++r.copies;
        start = r.nearHit.distance * 1.0001f + 1e-3f;
        r.nearHit.hit = false;
    }

    // Beyond the shadow map the shadow rays take cut-out geometry without the game's alpha as blocking
    // with chance: here always. Inside, they let the sun through it.
    r.farHit = TraceClosest(r.origin, r.direction, r.nearEnd, rayLength, GI_MASK_SCENE, true, r.beyond > 0.0f, PixelCone(position, 0.0f));
    r.anyHit = TraceFromSurface(r.origin, r.normal, r.tolerance, r.direction, rayLength, GI_MASK_SCENE, true, PixelCone(position, 0.0f));
    return r;
}

// The debug view: what blocks each pixel's sun ray. Lavender: one of the game's models or characters;
// brown: the terrain; yellow: cut-out geometry; olive: cut-out geometry without its texture, which the
// rays let the sun through inside the shadow map; magenta: anything else only the whole ray finds.
// Clear where the sun reaches. A cross marks the probe's pixel.
[numthreads(8, 8, 1)]
void DebugShadows(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    int2 fromCentre = int2(did) - int2(g_Frame.dimensions.xy / 2);
    if ((fromCentre.x == 0 && abs(fromCentre.y) <= 12) || (fromCentre.y == 0 && abs(fromCentre.x) <= 12))
    {
        g_ShadowDebug[did] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    float depth = g_Depth.Load(int3(did, 0));
    float4 color = 0.0f;
    if (depth > 0.0f)
    {
        float2 uv = (did + 0.5f) / g_Frame.dimensions.xy;
        float3 position = transformPointProjection(uv, depth, g_Frame.viewProjectionInverse);
        DebugRay r = TraceDebugRay(position, DecodeNormal(g_Normal.Load(int3(did, 0))), depth);
        SceneHit real = r.farHit;
        if (r.nearHit.hit) real = r.nearHit;
        if (real.hit)
        {
            uint kind = InstanceKind(g_Instances[real.instance]);
            SceneTriangle t = LoadTriangle(real.instance, real.geometry, real.primitive);
            if (t.geometry.flags & GI_GEOMETRY_CUT_OUT) color = float4(1.0f, 0.9f, 0.0f, 0.6f);
            else if (kind & GI_INSTANCE_TERRAIN) color = float4(0.75f, 0.5f, 0.25f, 0.6f);
            else color = float4(0.55f, 0.45f, 1.0f, 0.6f);
        }
        else if (r.anyHit.hit)
        {
            SceneTriangle t = LoadTriangle(r.anyHit.instance, r.anyHit.geometry, r.anyHit.primitive);
            color = (t.geometry.flags & GI_GEOMETRY_CUT_OUT) ? float4(0.5f, 0.45f, 0.0f, 0.6f) : float4(1.0f, 0.0f, 1.0f, 0.75f);
        }
    }

    g_ShadowDebug[did] = color;
}

groupshared float3 g_ProbeReceiver, g_ProbeNormal;
groupshared uint g_ProbeValid;
groupshared uint4 g_ProbeLightHits[GI_PROBE_LIGHT_RAYS];

// A hit as the probe lists it (GIShadowProbe::lightNearest); zeros for none.
uint4 ProbeHit(SceneHit hit)
{
    if (!hit.hit) return 0u;

    return uint4(hit.instance + 1, hit.geometry | (hit.frontFace ? 1u << 31 : 0u), InstanceKind(g_Instances[hit.instance]), asuint(hit.distance));
}

// The probe, one group: the centre pixel's sun ray, then its global illumination rays. Needs this
// frame's shadow passes done.
[numthreads(64, 1, 1)]
void ProbeShadows(uint index : SV_GroupIndex)
{
    if (index == 0)
    {
        g_ProbeValid = 0;

        GIShadowProbe probe = (GIShadowProbe)0;
        probe.terrain.w = float(g_Frame.dimensions.z);
        uint2 centre = g_Frame.dimensions.xy / 2;
        float depth = g_Depth.Load(int3(centre, 0));
        probe.receiver.w = depth;
        if (depth > 0.0f)
        {
            float2 uv = (centre + 0.5f) / g_Frame.dimensions.xy;
            float3 position = transformPointProjection(uv, depth, g_Frame.viewProjectionInverse);
            float3 normal = DecodeNormal(g_Normal.Load(int3(centre, 0)));
            DebugRay r = TraceDebugRay(position, normal, depth);
            probe.receiver.xyz = position;
            probe.normal = float4(normal, distance(position, g_Frame.eye.xyz));
            probe.ray = float4(r.direction, r.beyond);
            probe.segments = float4(r.nearEnd, r.tolerance, r.nearHit.hit ? r.nearHit.distance : -1.0f, r.farHit.hit ? r.farHit.distance : -1.0f);
            probe.hits = uint4(r.copies, r.nearHit.hit ? r.nearHit.instance + 1 : 0, r.farHit.hit ? r.farHit.instance + 1 : 0,
                r.anyHit.hit ? r.anyHit.instance + 1 : 0);
            if (r.anyHit.hit)
            {
                SceneTriangle t = LoadTriangle(r.anyHit.instance, r.anyHit.geometry, r.anyHit.primitive);
                uint kind = InstanceKind(g_Instances[r.anyHit.instance]);
                probe.hitGeometry = uint4(t.row, kind | (t.geometry.flags << 16), r.anyHit.geometry | (r.anyHit.frontFace ? 1u << 31 : 0u),
                    asuint(r.anyHit.distance));
                if (kind & GI_INSTANCE_TERRAIN)
                {
                    float3 corner = t.v0.y >= t.v1.y && t.v0.y >= t.v2.y ? t.v0 : t.v1.y >= t.v2.y ? t.v1 : t.v2;
                    probe.terrain.xyz = float3(corner.y, TerrainPoint(corner.xz).y, TerrainTile(TerrainUv(corner.xz)).z);
                }
            }

            // Where the rays start: the surface they leave, which side, and whatever the lift off it passes
            // through on the way (back from the start, stopping short of the surface itself).
            float3 lift = r.origin - r.surface.position;
            float liftLength = length(lift);
            probe.start = float4(r.surface.normal, r.surface.exact ? 1.0f : 0.0f);
            probe.lift = float4(lift, dot(r.normal, r.surface.normal) < 0.0f ? 1.0f : 0.0f);
            if (r.surface.exact)
                probe.startHit = uint4(r.surface.hit.instance + 1, r.surface.hit.geometry, r.surface.hit.primitive, r.anyHit.primitive);
            if (liftLength > 0.0f)
                probe.crossed = ProbeHit(TraceClosest(r.origin, -lift / liftLength, 0.0f, 0.95f * liftLength, GI_MASK_SCENE, true, false,
                    PixelCone(position, 0.0f)));

            uint tile = FFX_DNSR_Shadows_LinearTileIndex(FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(centre), g_Frame.dimensions.x);
            bool rayLit = (g_Shadow_Masks[tile] & FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(centre)) != 0;
            probe.shadow = float4(g_Shadow_Output[centre], rayLit ? 1.0f : 0.0f, 0.0f, 0.0f);
            // The camera's ray through the centre pixel, as the scene view traces it.
            float3 farPoint = transformPointProjection(uv, 1e-6f, g_Frame.viewProjectionInverse);
            probe.view = ProbeHit(TraceClosest(g_Frame.eye.xyz, normalize(farPoint - g_Frame.eye.xyz), 1e5f));
            g_ProbeReceiver = position;
            g_ProbeNormal = normal;
            g_ProbeValid = 1;
        }

        g_ShadowProbe[0] = probe;
    }

    GroupMemoryBarrierWithGroupSync();

    // The centre pixel's global illumination rays, as the probes trace theirs, spread evenly.
    if (index < GI_PROBE_LIGHT_RAYS)
    {
        uint4 lightHit = 0u;
        if (g_ProbeValid)
        {
            float2 spread = float2((index + 0.5f) / GI_PROBE_LIGHT_RAYS, frac(index * 0.618034f));
            float3 direction = mapToCosineHemisphere(spread, g_ProbeNormal);
            float3 origin = OffsetFromTriangle(g_ProbeReceiver, g_ProbeNormal);
            lightHit = ProbeHit(TraceFromSurface(origin, g_ProbeNormal, CopyTolerance(g_ProbeReceiver), direction, 1e9f));
        }

        g_ProbeLightHits[index] = lightHit;
    }

    GroupMemoryBarrierWithGroupSync();
    if (index == 0)
    {
        uint4 lightRays = 0u, nearest = 0u;
        for (uint ray = 0; ray < GI_PROBE_LIGHT_RAYS; ++ray)
        {
            uint4 hit = g_ProbeLightHits[ray];
            if (!hit.x)
            {
                ++lightRays.z;
                continue;
            }

            float hitDistance = asfloat(hit.w);
            lightRays += uint4(hitDistance < 1.0f, hitDistance < 10.0f, 0, (hit.y >> 31) == 0);
            if (!nearest.x || hitDistance < asfloat(nearest.w)) nearest = hit;
        }

        g_ShadowProbe[0].lightRays = lightRays;
        g_ShadowProbe[0].lightNearest = nearest;
    }
}
