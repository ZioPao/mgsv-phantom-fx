// The sun's ray-traced shadows (docs/DEVELOPMENT.md, "Ray-traced shadows"): a shadow ray from each
// pixel towards a point on the sun's disc, then the temporal pass of AMD's FidelityFX Shadow
// Denoiser (gi_shadow.hlsli); gi_shadow_filter.hlsl has its spatial passes.
//
// Cut-out geometry with the game's alpha (GI_GEOMETRY_ALPHA_TEXTURE) casts its own shadow, holes and
// all. The game's shadow map keeps the shadows of cut-out geometry without it where it reaches: the
// rays let the sun through it there, and the sun pass takes the darker of the two shadows. Beyond the
// shadow map, where the game has no shadow, the rays take it as letting half the sun through.
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
// (gi_common.hlsli, "Copies of a surface"); further on, cut-out geometry with the game's alpha blocks
// it where its alpha does, and other cut-out geometry with chance `blocking`, so the denoiser averages
// a partial shadow. Captured cut-out meshes take part only where `blocking` is above 0.
bool SunVisible(float3 origin, float3 normal, float tolerance, float3 direction, float blocking, float2 cone, inout Random random)
{
    float rayLength = g_Frame.shadowParams.z;
    float nearEnd = min(tolerance / max(dot(direction, normal), 0.1f), rayLength);
    if (TraceFromSurface(origin, normal, tolerance, direction, nearEnd, GI_MASK_SCENE, true, cone).hit) return false;
    if (nearEnd >= rayLength) return true;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = nearEnd;
    ray.TMax = rayLength;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;
    query.TraceRayInline(g_Scene, GI_RAY_FLAGS, blocking > 0.0f ? GI_MASK_TRACED : GI_MASK_SCENE, ray);
    // Only cut-out geometry is non-opaque, so only it comes here.
    while (query.Proceed())
    {
        uint row = query.CandidateInstanceContributionToHitGroupIndex() + query.CandidateGeometryIndex();
        float footprint = cone.x + query.CandidateTriangleRayT() * cone.y;
        if (CutOutBlocks(row, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), random.rand() < blocking, footprint,
                query.CandidateObjectToWorld3x4()))
            query.CommitNonOpaqueTriangleHit();
    }

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
            // Rays to the sun run parallel: each keeps its pixel's footprint. The sun's size comes from
            // spreading the rays over its disc; a cone widened by it as well took cut-out alpha from
            // coarse levels of detail, where a pylon's thin bars fell under the threshold.
            lit = SunVisible(OffsetFromScreenSurface(position, facing), facing, CopyTolerance(position), toSun, blocking, PixelCone(position, 0.0f), random);
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
// chance, with every instance looked at: the left-out ones (GI_MASK_HIDDEN) too.

RWStructuredBuffer<GIShadowProbe> g_ShadowProbe : register(u77);
RWStructuredBuffer<uint4> g_ShadowProbeCrossings : register(u79); // GI_PROBE_CROSSINGS (GIShadowProbe::crossings)
RWTexture2D<float4> g_ShadowDebug : register(u78);      // The debug view, blended over the image
StructuredBuffer<uint2> g_EntryStates : register(t11); // By captured mesh's entry (GI_ENTRY_*), from the CPU

struct DebugRay
{
    float3 origin, normal, direction;
    float tolerance, nearEnd, beyond;
    uint copies;      // Copies of the surface skipped in the near segment
    SceneHit nearHit; // The shadow rays' near segment, past copies
    SceneHit farHit;  // The shadow rays' far segment; beyond the shadow map, cut-out geometry without the game's alpha as opaque
    SceneHit anyHit;  // Every instance, the left-out too, past copies near the surface
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
        r.nearHit = TraceClosest(r.origin, r.direction, start, r.nearEnd, GI_MASK_SCENE, true, false, PixelCone(position, 0.0f));
        if (!r.nearHit.hit || !IsCopyOfSurface(r.nearHit, r.normal, r.direction, r.tolerance)) break;

        ++r.copies;
        start = r.nearHit.distance * 1.0001f + 1e-3f;
        r.nearHit.hit = false;
    }

    // Beyond the shadow map the shadow rays take cut-out geometry without the game's alpha as blocking
    // with chance: here always. Inside, they let the sun through it.
    r.farHit = TraceClosest(r.origin, r.direction, r.nearEnd, rayLength, r.beyond > 0.0f ? GI_MASK_TRACED : GI_MASK_SCENE, true, r.beyond > 0.0f, PixelCone(position, 0.0f));
    r.anyHit = TraceFromSurface(r.origin, r.normal, r.tolerance, r.direction, rayLength, 0xFFu, true, PixelCone(position, 0.0f));
    return r;
}

// The debug view: what blocks each pixel's sun ray. Lavender: one of the game's models; brown: the
// terrain; yellow: cut-out geometry; green: a captured mesh the game drew; cyan: one kept though not
// all in view; blue: one kept behind what the game drew (culled); red: a left-out one (only it blocks);
// olive: cut-out geometry the rays let the sun through inside the shadow map; magenta: anything else
// only the full ray finds. Clear where the sun reaches. A cross marks the probe's pixel.
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
            uint reason = g_InstanceDebug[real.instance].x;
            if (t.geometry.flags & GI_GEOMETRY_CUT_OUT) color = float4(1.0f, 0.9f, 0.0f, 0.6f);
            else if (kind & GI_INSTANCE_MODEL) color = float4(0.55f, 0.45f, 1.0f, 0.6f);
            else if (kind & GI_INSTANCE_TERRAIN) color = float4(0.75f, 0.5f, 0.25f, 0.6f);
            else if (reason == GI_REASON_OUT_OF_VIEW) color = float4(0.0f, 0.9f, 0.9f, 0.6f);
            else if (reason == GI_REASON_BEHIND) color = float4(0.1f, 0.3f, 1.0f, 0.6f);
            else color = float4(0.1f, 0.85f, 0.1f, 0.6f);
        }
        else if (r.anyHit.hit)
        {
            SceneTriangle t = LoadTriangle(r.anyHit.instance, r.anyHit.geometry, r.anyHit.primitive);
            uint reason = g_InstanceDebug[r.anyHit.instance].x;
            if (reason == GI_REASON_LEFT_OUT) color = float4(1.0f, 0.0f, 0.0f, 0.75f);
            else if (t.geometry.flags & GI_GEOMETRY_CUT_OUT) color = float4(0.5f, 0.45f, 0.0f, 0.6f);
            else color = float4(1.0f, 0.0f, 1.0f, 0.75f);
        }
    }

    g_ShadowDebug[did] = color;
}

groupshared float3 g_ProbeOrigin, g_ProbeDirection, g_ProbeReceiver, g_ProbeNormal;
groupshared uint g_ProbeValid, g_ProbeListed, g_ProbeCrossing, g_ProbeReasons[4];
groupshared uint4 g_ProbeLightHits[GI_PROBE_LIGHT_RAYS];

// A hit as the probe lists it (GIShadowProbe::lightNearest); zeros for none.
uint4 ProbeHit(SceneHit hit)
{
    if (!hit.hit) return 0u;

    return uint4(hit.instance + 1, hit.geometry | (hit.frontFace ? 1u << 31 : 0u), InstanceKind(g_Instances[hit.instance]), asuint(hit.distance));
}

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

// The probe, one group: the centre pixel's sun ray, then the instances by reason and the captured
// meshes whose bounds the ray crosses (g_Dispatch.x entries). Needs this frame's shadow passes done.
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
                GIInstanceDesc instance = g_Instances[r.anyHit.instance];
                SceneTriangle t = LoadTriangle(r.anyHit.instance, r.anyHit.geometry, r.anyHit.primitive);
                uint kind = InstanceKind(instance);
                bool captured = (kind & (GI_INSTANCE_CAPTURED | GI_INSTANCE_DYNAMIC)) != 0;
                uint4 debug = g_InstanceDebug[r.anyHit.instance];
                probe.hitEntry = uint4(captured ? InstanceEntry(instance) : t.row, debug.x, kind | (t.geometry.flags << 16),
                    captured ? g_EntryStates[InstanceEntry(instance)].x : 0u);
                probe.hitBox = float4(asfloat(debug.y), asfloat(debug.z), float(debug.w & 255u), (debug.w & 256u) ? 1.0f : 0.0f);
                if (kind & GI_INSTANCE_TERRAIN)
                {
                    float3 corner = t.v0.y >= t.v1.y && t.v0.y >= t.v2.y ? t.v0 : t.v1.y >= t.v2.y ? t.v1 : t.v2;
                    probe.terrain.xyz = float3(corner.y, TerrainPoint(corner.xz).y, TerrainTile(TerrainUv(corner.xz)).z);
                }
            }

            uint tile = FFX_DNSR_Shadows_LinearTileIndex(FFX_DNSR_Shadows_GetTileIndexFromPixelPosition(centre), g_Frame.dimensions.x);
            bool rayLit = (g_Shadow_Masks[tile] & FFX_DNSR_Shadows_GetBitMaskFromPixelPosition(centre)) != 0;
            probe.shadow = float4(g_Shadow_Output[centre], rayLit ? 1.0f : 0.0f, 0.0f, 0.0f);
            // The camera's ray through the centre pixel, as the scene view traces it.
            float3 farPoint = transformPointProjection(uv, 1e-6f, g_Frame.viewProjectionInverse);
            probe.view = ProbeHit(TraceClosest(g_Frame.eye.xyz, normalize(farPoint - g_Frame.eye.xyz), 1e5f));
            g_ProbeOrigin = r.origin;
            g_ProbeDirection = r.direction;
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
