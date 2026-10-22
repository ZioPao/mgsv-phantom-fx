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
