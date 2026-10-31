// Bindings and the denoiser's data access for the sun's ray-traced shadows (gi_shadow.hlsl,
// gi_shadow_filter.hlsl; docs/DEVELOPMENT.md, "Ray-traced shadows"). The denoiser is AMD's
// FidelityFX Shadow Denoiser (third_party/ffx_denoiser_shadows, MIT; licenses/AMD-FidelityFX-LICENSE.txt),
// whose Read, Write and Get functions this file provides.
#ifndef GI_SHADOW_HLSLI
#define GI_SHADOW_HLSLI

#include "gi_common.hlsli"

#define INVERTED_DEPTH_RANGE // Reversed-Z

RWStructuredBuffer<uint> g_Shadow_Masks : register(u80);        // A bit a pixel in 8x4 pixel tiles: 1 where the ray reached the sun
RWStructuredBuffer<uint> g_Shadow_TileMetadata : register(u81); // An 8x8 pixel tile each: TILE_META_DATA_*
RWTexture2D<float2> g_Shadow_Reprojection : register(u82);      // Mean and variance: after the temporal pass, then after the second filter
RWTexture2D<float2> g_Shadow_History : register(u83);           // After the first filter: the next frame's history
RWTexture2D<float3> g_Shadow_Moments[2] : register(u84);        // u84, u85: this frame's, the last frame's; mean, variance, samples
RWTexture2D<float> g_Shadow_Depths[2] : register(u86);          // u86, u87: this frame's depth, the last frame's
RWTexture2D<float> g_Shadow_Output : register(u88);             // What the sun pass reads: 1 where the sun reaches
int2 ShadowSize()
{
    return int2(g_Frame.dimensions.xy);
}

int2 ClampToShadow(int2 pixel)
{
    return clamp(pixel, 0, ShadowSize() - 1);
}

// ---- The denoiser's data access ----

int2 FFX_DNSR_Shadows_GetBufferDimensions()
{
    return ShadowSize();
}

float2 FFX_DNSR_Shadows_GetInvBufferDimensions()
{
    return 1.0f / float2(ShadowSize());
}

// Its linear depth is abs(z / w) of this times (ndc, depth, 1): a matrix whose z is 1 and whose w is
// 1 over toLinearDepth, as Fox Engine's projection is not kept on its own.
float4x4 FFX_DNSR_Shadows_GetProjectionInverse()
{
    float nearPlane = g_Frame.nearFar.x, farPlane = g_Frame.nearFar.y;
    return float4x4(1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f,
                    0.0f, 0.0f, (farPlane - nearPlane) / (nearPlane * farPlane), 1.0f / farPlane);
}

float4x4 FFX_DNSR_Shadows_GetViewProjectionInverse()
{
    return g_Frame.viewProjectionInverse;
}

// This frame's (ndc, depth, 1) to the last frame's clip space.
float4x4 FFX_DNSR_Shadows_GetReprojectionMatrix()
{
    return mul(g_Frame.previousViewProjection, g_Frame.viewProjectionInverse);
}

float3 FFX_DNSR_Shadows_GetEye()
{
    return g_Frame.eye.xyz;
}

uint FFX_DNSR_Shadows_IsFirstFrame()
{
    return g_Frame.shadowParams.w != 0.0f ? 1u : 0u;
}

float FFX_DNSR_Shadows_ReadDepth(int2 pixel)
{
    return g_Depth.Load(int3(ClampToShadow(pixel), 0));
}

float FFX_DNSR_Shadows_ReadPreviousDepth(int2 pixel)
{
    return g_Shadow_Depths[1][ClampToShadow(pixel)];
}

float3 FFX_DNSR_Shadows_ReadNormals(int2 pixel)
{
    return DecodeNormal(g_Normal.Load(int3(ClampToShadow(pixel), 0)));
}

bool FFX_DNSR_Shadows_IsShadowReciever(uint2 pixel)
{
    return all(pixel < uint2(ShadowSize())) && FFX_DNSR_Shadows_ReadDepth(int2(pixel)) > 0.0f;
}

// The motion from the last frame, in NDC, this frame's minus the last's: a moving mesh's from the game's
// velocity (which carries this frame's jitter, as the camera's motion here does), elsewhere the
// camera's, the depth's world position through the last frame's projection-view.
float2 FFX_DNSR_Shadows_ReadVelocity(int2 pixel)
{
    float2 moved;
    if (ObjectMotion(ClampToShadow(pixel), moved)) return moved * float2(2.0f, -2.0f) * FFX_DNSR_Shadows_GetInvBufferDimensions();

    float2 uv = (ClampToShadow(pixel) + 0.5f) * FFX_DNSR_Shadows_GetInvBufferDimensions();
    float2 ndc = 2.0f * float2(uv.x, 1.0f - uv.y) - 1.0f;
    float4 previous = mul(FFX_DNSR_Shadows_GetReprojectionMatrix(), float4(ndc, FFX_DNSR_Shadows_ReadDepth(pixel), 1.0f));
    return ndc - previous.xy / previous.w;
}

uint FFX_DNSR_Shadows_ReadRaytracedShadowMask(uint tile)
{
    return g_Shadow_Masks[tile];
}

void FFX_DNSR_Shadows_WriteMetadata(uint tile, uint mask)
{
    g_Shadow_TileMetadata[tile] = mask;
}

uint FFX_DNSR_Shadows_ReadTileMetaData(uint tile)
{
    return g_Shadow_TileMetadata[tile];
}

void FFX_DNSR_Shadows_WriteReprojectionResults(uint2 pixel, float2 meanAndVariance)
{
    g_Shadow_Reprojection[pixel] = meanAndVariance;
}

void FFX_DNSR_Shadows_WriteMoments(uint2 pixel, float3 moments)
{
    g_Shadow_Moments[0][pixel] = moments;
}

float3 FFX_DNSR_Shadows_ReadPreviousMomentsBuffer(int2 pixel)
{
    return g_Shadow_Moments[1][ClampToShadow(pixel)];
}

// The history is read bilinearly, as through the denoiser's linear sampler.
float FFX_DNSR_Shadows_ReadHistory(float2 uv)
{
    float2 position = uv * ShadowSize() - 0.5f;
    int2 corner = int2(floor(position));
    float2 weight = position - corner;
    float top = lerp(g_Shadow_History[ClampToShadow(corner)].x, g_Shadow_History[ClampToShadow(corner + int2(1, 0))].x, weight.x);
    float bottom = lerp(g_Shadow_History[ClampToShadow(corner + int2(0, 1))].x, g_Shadow_History[ClampToShadow(corner + int2(1, 1))].x, weight.x);
    return lerp(top, bottom, weight.y);
}

// ---- Where a pixel's shadow rays leave its surface ----
// The scene's own surface where a ray from the eye through the pixel meets it near the depth's point: its
// exact point and its triangle's normal, from which a ray leaves past that very surface (LiftOffTriangle),
// so nothing near it needs skipping.
// Where the game drew something the scene lacks, holds at another level of detail, or only shares a depth
// with (a hole the game draws as a normal map, a stand-in's facet), the depth's point and the G-buffer's
// normal, with copies of the surface skipped (gi_common.hlsli). Until 2026-10-24 every ray left that way:
// the skip took a character pressed to a wall for a copy of the wall, and light went through him, ragged
// along the wall's bumpy normals.
struct ShadowSurface
{
    float3 position;
    float3 normal; // Towards the eye: the triangle's on the scene's surface, else the G-buffer's
    bool exact;    // On the scene's own surface
    SceneHit hit;  // Diagnostics (the probe): the eye's ray's hit on it
};

// How far the scene's surface may face from the game's normal and still be the surface the rays leave:
// the shadow ray starts view's blue, 60 degrees (ShadowStartColor). A triangle that only shares the depth's
// point is not what the game drew there, and its rays left lit where the game has shade: a wall's dimples,
// drawn as a normal map, glowed from their own facets (2026-10-30).
static const float SceneSurfaceNormalCosine = 0.5f;

// How far off the depth's point the scene's triangle may lie and still be the surface the game drew
// there. Recessed facets (a dimple the game draws as a normal map, another level of detail) pass the
// depth's tolerance but let the ray out from behind the surface, lit where the game has shade.
static const float SceneSurfacePointDistance = 0.01f;



ShadowSurface ShadowSurfaceAt(float3 position, float3 gbufferNormal)
{
    ShadowSurface surface;
    surface.position = position;
    surface.normal = gbufferNormal;
    surface.exact = false;
    surface.hit = (SceneHit)0;

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
    // A stand-in the camera never sees is not the surface (a tarp's slab, 16 cm before its sheet).
    if (!hit.hit || abs(hit.distance - surfaceDistance) > tolerance || IsShadowOnly(hit.instance, hit.geometry)) return surface;

    SceneTriangle t = LoadTriangle(hit.instance, hit.geometry, hit.primitive);
    float3 normal = cross(t.v1 - t.v0, t.v2 - t.v0);
    float lengthSquared = dot(normal, normal);
    if (!(lengthSquared > 0.0f)) return surface;

    normal *= rsqrt(lengthSquared);
    float3 facing = dot(normal, direction) > 0.0f ? -normal : normal;
    if (dot(facing, gbufferNormal) < SceneSurfaceNormalCosine) return surface;

    // The ray leaves the triangle's own point only where that point is the depth's, as on a character's
    // folds (the fold fix of 2026-10-24 rests on it). A triangle off it is a facet recessed in the surface
    // the game drew — a dimple's wall, another level of detail — and its ray left from behind that
    // surface, lit where the game has shade (2026-10-30). It keeps the triangle's normal, and the ray
    // leaves the depth's point, skipping copies as where the scene lacks the surface.
    float3 trianglePoint = TrianglePoint(t, hit.barycentrics);
    bool onDepthPoint = distance(trianglePoint, position) <= SceneSurfacePointDistance;
    surface.position = onDepthPoint ? trianglePoint : position;
    surface.normal = facing;
    surface.exact = onDepthPoint;
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

// A shadow ray's start towards `toLight`: from the surface's side facing the light (`facing`), as leaves
// lit from behind take light through them. Its point, though, is offset towards the eye, never along that
// side's normal: where the light is behind the surface, facing flips away from the eye, and the start then
// lay behind the wall the camera sees — where the sun's rays met its back, which they pass (StopsShadowRay)
// — and a wall's dimples, whose normals point away from the sun, took sun the game has shade there
// (2026-10-30). On the scene's own surface the lift does the same, unless the geometry is thin (cut-out,
// leaves), which keeps the light's side so it does not shadow itself.
void ShadowRayStart(ShadowSurface surface, float3 toLight, out float3 origin, out float3 facing, out float tolerance)
{
    facing = dot(surface.normal, toLight) >= 0.0f ? surface.normal : -surface.normal;
    // Towards the eye, where a normal map has turned the fallback's normal past the camera.
    float3 startSide = dot(surface.normal, surface.position - g_Frame.eye.xyz) > 0.0f ? -surface.normal : surface.normal;
    if (surface.exact)
    {
        // An opaque surface the light lies behind blocks its own ray: lift towards the eye, so the ray
        // meets the surface's front and is stopped — the shade a wall gives its own far side. Before, the
        // lift went along the light's side, 2.8 mm behind the wall, and with nothing between it and the
        // light the spot read lit where the game has shade (the shadow probe, 2026-10-30). Thin geometry —
        // cut-out, leaves — keeps the light's side, so it does not shadow itself.
        bool thin = surface.hit.hit
            && (g_Geometries[FirstGeometry(g_Instances[surface.hit.instance]) + surface.hit.geometry].flags & (GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_LEAF)) != 0;
        origin = LiftOffTriangle(surface.position, thin ? facing : startSide);
    }
    else origin = OffsetFromScreenSurface(surface.position, startSide);
    tolerance = surface.exact ? 0.0f : CopyTolerance(surface.position);
}

// A hit as the probe lists it (GIShadowProbe::lightNearest); zeros for none.
uint4 ProbeHit(SceneHit hit)
{
    if (!hit.hit) return 0u;

    return uint4(hit.instance + 1, hit.geometry | (hit.frontFace ? 1u << 31 : 0u), InstanceKind(g_Instances[hit.instance]), asuint(hit.distance));
}

#endif
