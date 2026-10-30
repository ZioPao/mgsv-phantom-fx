// Path-traced reflections (docs/DEVELOPMENT.md, "Path tracing"): with DLSS Ray Reconstruction denoising,
// a ray from every render pixel along a reflection its surface's roughness spreads (GGX's visible normals),
// its hit lit as the global illumination's rays light theirs: the sun and a light afresh, its bounce from
// the sky or the light cache. The ray that gets away takes the sky's light; the sun's disc stays the sun
// pass's specular. The light goes out times the surface's specular albedo (the Ray Reconstruction guide)
// and the exposure, for the resolve to add to the game's specular light, and the hit distance goes to
// Ray Reconstruction to move the reflection with what it reflects.
#include "gi_hit_light.hlsli"

Texture2D<float4> g_Guide_NormalRoughness : register(t18); // The world-space normal, the roughness in w (ray_tracing::ReconstructionGuides)
Texture2D<float4> g_Guide_SpecularAlbedo : register(t19);  // The share of light the surface reflects towards the eye
RWTexture2D<float4> g_Reflection : register(u93);          // The reflected light times the specular albedo and the exposure
RWTexture2D<float> g_ReflectionDistance : register(u94);   // The ray's hit distance; FP16's largest where it got away

// FP16's largest, which Ray Reconstruction takes as the sky.
static const float SkyHitDistance = 65504.0f;

// A visible normal of GGX of roughness `alpha` around +z, seen from `view` (Heitz, "Sampling the GGX
// Distribution of Visible Normals", JCGT 2018).
float3 SampleVisibleNormal(float3 view, float alpha, float2 u)
{
    float3 stretched = normalize(float3(alpha * view.xy, view.z));
    float lengthSquared = dot(stretched.xy, stretched.xy);
    float3 t1 = lengthSquared > 0.0f ? float3(-stretched.y, stretched.x, 0.0f) * rsqrt(lengthSquared) : float3(1.0f, 0.0f, 0.0f);
    float3 t2 = cross(stretched, t1);
    float r = sqrt(u.x);
    float sinPhi, cosPhi;
    sincos(TWO_PI * u.y, sinPhi, cosPhi);
    float p1 = r * cosPhi;
    float s = 0.5f * (1.0f + stretched.z);
    float p2 = (1.0f - s) * sqrt(1.0f - p1 * p1) + s * r * sinPhi;
    float3 normal = p1 * t1 + p2 * t2 + sqrt(max(0.0f, 1.0f - p1 * p1 - p2 * p2)) * stretched;
    return normalize(float3(alpha * normal.xy, max(normal.z, 0.0f)));
}

// The light along a ray, as the global illumination's other rays than the first find theirs (TraceLight):
// the hit's own light from the sun and a light, and its bounce's from the sky or the cache as it stands.
float3 ReflectedLight(float3 origin, float3 normal, float tolerance, float3 direction, inout Random random, out float hitDistance)
{
    SceneHit hit = TraceFromSurface(origin, normal, tolerance, direction, MAX_HIT_DISTANCE);
    if (!hit.hit)
    {
        hitDistance = SkyHitDistance;
        return SkyRadiance(direction);
    }

    hitDistance = min(hit.distance, SkyHitDistance);
    HashGridCache_Visibility visibility = VisibilityOf(hit, direction);
    float3 hitWorld, hitNormal;
    HitSurface(visibility, hitWorld, hitNormal);
    float3 albedo = HitAlbedo(hitWorld, visibility);
    bool isLeaf = HitIsLeaf(visibility);
    float3 hitOrigin = OffsetFromTriangle(hitWorld, hitNormal);
    float hitTolerance = CopyTolerance(hitWorld);
    float3 light = albedo * (SunLight(hitWorld, hitNormal, hitOrigin, hitTolerance, isLeaf, random)
        + SampledLocalLight(hitWorld, hitNormal, hitOrigin, hitTolerance, random));

    float weight;
    float3 side = BounceSide(hitNormal, isLeaf, random, weight);
    float3 bounceDirection = mapToCosineHemisphere(random.rand2(), side);
    float3 bounceOrigin = all(side == hitNormal) ? hitOrigin : OffsetFromTriangle(hitWorld, side);
    SceneHit bounce = TraceFromSurface(bounceOrigin, side, hitTolerance, bounceDirection, MAX_HIT_DISTANCE);
    float3 throughput = albedo * weight;
    if (!bounce.hit) return light + throughput * SkyRadiance(bounceDirection);

    float3 bounceHit = bounceOrigin + bounce.distance * bounceDirection;
    uint tileIndex;
    bool isNewTile;
    uint cellIndex = HashGridCache_InsertCell(bounceHit, bounceDirection, bounce.distance, tileIndex, isNewTile);
    if (cellIndex == kGI1_InvalidId) return light;

    HashGridCache_TouchCell(tileIndex, isNewTile);
    return light + throughput * (HashGridCache_FilteredDirect(cellIndex) + HashGridCache_FilteredIndirect(cellIndex));
}

[numthreads(8, 8, 1)]
void TraceReflections(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_BufferDimensions)) return;

    float depth = g_Depth.Load(int3(did, 0));
    float3 specularAlbedo = g_Guide_SpecularAlbedo.Load(int3(did, 0)).rgb;
    if (depth <= 0.0f || all(specularAlbedo < 1e-3f))
    {
        g_Reflection[did] = 0.0f;
        g_ReflectionDistance[did] = depth <= 0.0f ? SkyHitDistance : 0.0f;
        return;
    }

    float4 normalRoughness = g_Guide_NormalRoughness.Load(int3(did, 0));
    float3 normal = normalize(normalRoughness.xyz);
    float alpha = max(normalRoughness.w * normalRoughness.w, 1e-3f);
    float3 world = DepthPosition(did, depth);
    float3 toEye = normalize(g_Frame.eye.xyz - world);

    // In the surface's frame, the eye above it: a normal map can tilt the normal away from the eye.
    float3 tangent, bitangent;
    GetOrthoVectors(normal, tangent, bitangent);
    float3 view = normalize(float3(dot(toEye, tangent), dot(toEye, bitangent), max(dot(toEye, normal), 1e-3f)));
    // Apart from the global illumination's rays from the same pixel (TraceLight seeds by the pixel alone).
    Random random = MakeRandom(did.x + did.y * g_BufferDimensions.x + g_BufferDimensions.x * g_BufferDimensions.y, g_FrameIndex);
    float3 halfway = SampleVisibleNormal(view, alpha, random.rand2());
    float3 reflected = reflect(-view, halfway);
    // Below the surface: the mirror direction in its place.
    if (reflected.z <= 0.0f) reflected = float3(-view.xy, view.z);

    float3 direction = normalize(reflected.x * tangent + reflected.y * bitangent + reflected.z * normal);
    float3 origin = OffsetFromScreenSurface(world, normal);
    float hitDistance;
    float3 light = ReflectedLight(origin, normal, CopyTolerance(world), direction, random, hitDistance);
    g_Reflection[did] = float4(RemoveNaNs(light) * specularAlbedo * g_Frame.nearFar.w, 1.0f);
    g_ReflectionDistance[did] = hitDistance;
}
