// The sun's ray-traced shadows (docs/DEVELOPMENT.md, "Ray-traced shadows"): a shadow ray from each
// pixel towards a point on the sun's disc, then the temporal pass of AMD's FidelityFX Shadow
// Denoiser (gi_shadow.hlsli); gi_shadow_filter.hlsl has its spatial passes.
//
// The game's shadow map keeps the alpha-tested meshes' shadows (leaves, grass, nets) where it
// reaches: the scene holds their triangles without the holes their textures cut, and grass not at
// all. There the rays leave them out, and the sun pass takes the darker of the two shadows. Beyond
// the shadow map, where the game has no shadow, the rays take them as letting half the sun through.
//
// Where it deviates from the denoiser's sample, and why:
// - The trace writes the denoiser's 8x4 tile masks itself, through groupshared memory, which works
//   at any wave size.
// - Motion comes from the camera (gi_shadow.hlsli, ReadVelocity).
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

// Whether a ray leaving a surface reaches the sun. Near the surface, copies of it are skipped
// (gi_common.hlsli, "Copies of a surface"); further on, each alpha-tested mesh the ray passes blocks
// it with chance `blocking`, so the denoiser averages a partial shadow.
bool SunVisible(float3 origin, float3 normal, float tolerance, float3 direction, float blocking, inout Random random)
{
    float rayLength = g_Frame.shadowParams.z;
    float nearEnd = min(tolerance / max(dot(direction, normal), 0.1f), rayLength);
    if (TraceFromSurface(origin, normal, tolerance, direction, nearEnd, GI_MASK_OPAQUE).hit) return false;
    if (nearEnd >= rayLength) return true;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = nearEnd;
    ray.TMax = rayLength;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;
    query.TraceRayInline(g_Scene, RAY_FLAG_NONE, blocking > 0.0f ? GI_MASK_OPAQUE | GI_MASK_ALPHA_TESTED : GI_MASK_OPAQUE, ray);
    // Only the alpha-tested instances are non-opaque, so only they come here.
    while (query.Proceed())
        if (random.rand() < blocking) query.CommitNonOpaqueTriangleHit();

    return query.CommittedStatus() == COMMITTED_NOTHING;
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
            // Leaves turned away from the sun still let it through; the ray leaves from their sunlit side.
            float3 facing = dot(normal, toSun) >= 0.0f ? normal : -normal;
            float blocking = BeyondShadowMap(position, depth) * g_Frame.shadowParams.y;
            lit = SunVisible(OffsetFromScreenSurface(position, facing), facing, CopyTolerance(position), toSun, blocking, random);
        }
    }

    if (lit) InterlockedOr(g_TileMask, FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(did));
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) g_Shadow_Masks[FFX_DNSR_Shadows_LinearTileIndex(gid, g_Frame.dimensions.x)] = g_TileMask;
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
// chance, with every instance looked at: the left-out ones (GI_MASK_HIDDEN) and the alpha-tested
// ones the shadow rays leave to the shadow map.

RWStructuredBuffer<GIShadowProbe> g_ShadowProbe : register(u77);
RWStructuredBuffer<uint4> g_ShadowProbeCrossings : register(u79); // GI_PROBE_CROSSINGS (GIShadowProbe::crossings)
RWTexture2D<float4> g_ShadowDebug : register(u78);      // The debug view, blended over the image
StructuredBuffer<uint2> g_EntryStates : register(t11); // By entry (GI_ENTRY_*), from the CPU

struct DebugRay
{
    float3 origin, normal, direction;
    float tolerance, nearEnd, beyond;
    uint copies;      // Copies of the surface skipped in the near segment
    SceneHit nearHit; // The shadow rays' near segment: opaque instances, past copies
    SceneHit farHit;  // The shadow rays' far segment: alpha-tested too beyond the shadow map, as opaque
    SceneHit anyHit;  // Every instance, past copies near the surface
};

DebugRay TraceDebugRay(float3 position, float3 normal, float depth)
{
    DebugRay r;
    r.direction = g_Frame.sunDirection.xyz;
    r.normal = dot(normal, r.direction) >= 0.0f ? normal : -normal;
    r.origin = OffsetFromScreenSurface(position, r.normal);
    r.tolerance = CopyTolerance(position);
    r.beyond = BeyondShadowMap(position, depth);
    float rayLength = g_Frame.shadowParams.z;
    r.nearEnd = min(r.tolerance / max(dot(r.direction, r.normal), 0.1f), rayLength);

    // As TraceFromSurface, counting the copies it skips.
    r.copies = 0;
    float start = 0.0f;
    for (uint attempt = 0; attempt < 4; ++attempt)
    {
        r.nearHit = TraceClosest(r.origin, r.direction, start, r.nearEnd, GI_MASK_OPAQUE);
        if (!r.nearHit.hit || !IsCopyOfSurface(r.nearHit, r.normal, r.direction, r.tolerance)) break;

        ++r.copies;
        start = r.nearHit.distance * 1.0001f + 1e-3f;
        r.nearHit.hit = false;
    }

    uint farMask = r.beyond > 0.0f ? GI_MASK_OPAQUE | GI_MASK_ALPHA_TESTED : GI_MASK_OPAQUE;
    r.farHit = TraceClosest(r.origin, r.direction, r.nearEnd, rayLength, farMask);
    r.anyHit = TraceFromSurface(r.origin, r.normal, r.tolerance, r.direction, rayLength, 0xFFu);
    return r;
}

// The debug view: what blocks each pixel's sun ray. Green: an instance the game drew; cyan: one kept
// though not all in view; blue: one kept behind what the game drew (culled); yellow: an alpha-tested
// one the rays take beyond the shadow map; olive: an alpha-tested one left to the shadow map; red: a
// left-out one (only it blocks); magenta: anything else only the full ray finds. Clear where the sun
// reaches. A cross marks the probe's pixel.
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
            GIInstanceRecord instance = g_InstanceRecords[real.instance];
            uint reason = g_InstanceDebug[real.instance].x;
            if (instance.flags & GI_INSTANCE_ALPHA_TESTED) color = float4(1.0f, 0.9f, 0.0f, 0.6f);
            else if (reason == GI_REASON_OUT_OF_VIEW) color = float4(0.0f, 0.9f, 0.9f, 0.6f);
            else if (reason == GI_REASON_BEHIND) color = float4(0.1f, 0.3f, 1.0f, 0.6f);
            else color = float4(0.1f, 0.85f, 0.1f, 0.6f);
        }
        else if (r.anyHit.hit)
        {
            GIInstanceRecord instance = g_InstanceRecords[r.anyHit.instance];
            uint reason = g_InstanceDebug[r.anyHit.instance].x;
            if (reason == GI_REASON_LEFT_OUT) color = float4(1.0f, 0.0f, 0.0f, 0.75f);
            else if (instance.flags & GI_INSTANCE_ALPHA_TESTED) color = float4(0.5f, 0.45f, 0.0f, 0.6f);
            else color = float4(1.0f, 0.0f, 1.0f, 0.75f);
        }
    }

    g_ShadowDebug[did] = color;
}

groupshared float3 g_ProbeOrigin, g_ProbeDirection;
groupshared uint g_ProbeValid, g_ProbeListed, g_ProbeCrossing, g_ProbeReasons[4];

// Where a ray from `origin` along `direction` enters the box, or a negative value when it misses it
// within `rayLength`.
float EnterBox(float3 origin, float3 direction, float3 lowest, float3 highest, float rayLength)
{
    float3 inverse = 1.0f / select(abs(direction) > 1e-8f, direction, 1e-8f);
    float3 t0 = (lowest - origin) * inverse, t1 = (highest - origin) * inverse;
    float enter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), max(min(t0.z, t1.z), 0.0f));
    float leave = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), min(max(t0.z, t1.z), rayLength));
    return enter <= leave ? enter : -1.0f;
}

// The probe, one group: the centre pixel's sun ray, then the instances by reason and the live
// entries whose bounds the ray crosses (g_Dispatch.x entries). Needs this frame's shadow passes done.
[numthreads(64, 1, 1)]
void ProbeShadows(uint index : SV_GroupIndex)
{
    if (index < 4) g_ProbeReasons[index] = 0;
    if (index == 0)
    {
        g_ProbeListed = 0;
        g_ProbeCrossing = 0;
        g_ProbeValid = 0;

        GIShadowProbe probe = (GIShadowProbe)0;
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
                GIInstanceRecord instance = g_InstanceRecords[r.anyHit.instance];
                uint4 debug = g_InstanceDebug[r.anyHit.instance];
                probe.hitEntry = uint4(instance.entry, debug.x, instance.flags, g_EntryStates[instance.entry].x);
                probe.hitBox = float4(asfloat(debug.y), asfloat(debug.z), float(debug.w & 255u), (debug.w & 256u) ? 1.0f : 0.0f);
            }

            uint tile = FFX_DNSR_Shadows_LinearTileIndex(FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(centre), g_Frame.dimensions.x);
            bool rayLit = (g_Shadow_Masks[tile] & FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(centre)) != 0;
            probe.shadow = float4(g_Shadow_Output[centre], rayLit ? 1.0f : 0.0f, 0.0f, 0.0f);
            g_ProbeOrigin = r.origin;
            g_ProbeDirection = r.direction;
            g_ProbeValid = 1;
        }

        g_ShadowProbe[0] = probe;
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint i = index; i < g_Frame.dimensions.w; i += 64)
    {
        uint reason = g_InstanceDebug[i].x;
        if (reason < 4) InterlockedAdd(g_ProbeReasons[reason], 1);
    }

    if (g_ProbeValid)
        for (uint entry = index; entry < g_Dispatch.x; entry += 64)
        {
            uint2 state = g_EntryStates[entry];
            if ((state.x & GI_ENTRY_LIVE) == 0) continue;

            float3 lowest, highest;
            for (uint axis = 0; axis < 3; ++axis)
            {
                lowest[axis] = FloatFromOrdered(g_EntryBounds[6 * entry + axis]);
                highest[axis] = FloatFromOrdered(g_EntryBounds[6 * entry + 3 + axis]);
            }

            float enter = EnterBox(g_ProbeOrigin, g_ProbeDirection, lowest, highest, g_Frame.shadowParams.z);
            if (enter < 0.0f) continue;

            uint slot;
            InterlockedAdd(g_ProbeCrossing, 1, slot);
            if (slot >= GI_PROBE_CROSSINGS) continue;

            uint reason = state.y != GI_REASON_NONE ? g_InstanceDebug[state.y].x : GI_REASON_NONE;
            g_ShadowProbeCrossings[slot] = uint4(entry, asuint(enter), state.x, reason);
            InterlockedAdd(g_ProbeListed, 1);
        }

    GroupMemoryBarrierWithGroupSync();
    if (index == 0)
    {
        g_ShadowProbe[0].reasons = uint4(g_ProbeReasons[0], g_ProbeReasons[1], g_ProbeReasons[2], g_ProbeReasons[3]);
        g_ShadowProbe[0].crossings = uint4(g_ProbeCrossing, g_ProbeListed, 0, 0);
    }
}
