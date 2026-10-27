// The global illumination's indirect light (docs/DEVELOPMENT.md, "Global illumination"), as path tracers
// with a radiance cache work it out:
// - Rays from each pixel of a grid half the render size each way (1, 2 or 4, the ray quality, twice on
//   foliage), from one render pixel of its 2 x 2 chosen by depth (RenderPixelOf), cosine-weighted from the
//   surface the depth buffer gives (TraceLight).
// - Where it hits, the hit is lit afresh every frame: a shadow ray to a point of the sun's disc, and
//   one to a point or spot light picked in proportion to its light there (weighted reservoir
//   sampling). Its bounce ray takes the sky's light where it gets away, and otherwise the light of the
//   surface it reaches from a world-space radiance cache: AMD's two-level hash grid of GI-1.2 (Capsaicin,
//   MIT; licenses/Capsaicin-LICENSE.txt), whose cells hold the light at second hits and, through their
//   own rays, one bounce more. The cache only ends the path, where NVIDIA's SHaRC sits in a path tracer:
//   its lag (a cell takes 1 / 16 of a frame's light) and its coarseness come after a bounce, not in the
//   light a pixel sees first.
// - Foliage's light mostly from cells in the world that keep it over the frames (RememberFoliageLight).
// - NVIDIA's RELAX denoiser in its spherical harmonics form (gi_denoiser.cpp) at the same grid, given the
//   motion without the upscaler's jitter and the game's foliage as strands, then the render size from the four light pixels around each render pixel, weighed by how alike their surfaces
//   are and resolved against the render pixel's own normal (CompositeLight).
//
// Where it deviates from a path tracer, and why:
// - A hit's albedo is the G-buffer's where the hit is on screen; otherwise the game's albedo texture
//   for a model's mesh (taken from its G-buffer draw, engine_scene.hpp), and the mesh's mean albedo,
//   learnt from the screen (LearnAlbedo), for the rest.
// - The lights are the sun and the sky, as the game's own sky light gives them, and the frame's point
//   and spot lights. Emissive surfaces are left out. Specular light is left out: the game keeps its own.
// - Rays from a surface skip copies of it just above (gi_common.hlsli, "Copies of a surface").
// - A hash grid tile is keyed by the side of the surface a ray hits (the ray's main axis and its
//   sign), not by its finely quantised direction: diffuse light needs no more, and the grid fills
//   up far less (a hit without room gives no light).
// - Only the first few hits of a cell each frame bring it light (HashGridCache_QueueLight); dozens a
//   frame land in most cells and a cell keeps 16 samples.
#include "gi_common.hlsli"
#include "NRD.hlsli"

// ---- Bindings, after gi_common.hlsli's ----

RWTexture2D<float> g_Light_ViewZ : register(u7);            // The denoiser's guides: NRD's IN_VIEWZ,
RWTexture2D<float4> g_Light_NormalRoughness : register(u8); // IN_NORMAL_ROUGHNESS,
RWTexture2D<float> g_Light_LastViewZ : register(u30);       // g_Light_ViewZ as the last frame wrote it (PrepareGuides)
RWTexture2D<float4> g_Light_Motion : register(u9);          // IN_MV: the last frame's uv minus this one's, and view z's change
RWTexture2D<float4> g_Light_Noisy[2] : register(u10);       // u10, u11: NRD's IN_DIFF_SH0 and SH1
RWTexture2D<float4> g_Light_Denoised[2] : register(u12);    // u12, u13: NRD's OUT_DIFF_SH0 and SH1
RWTexture2D<float4> g_Light_Partial : register(u14);        // A ray's light but its bounce's cache: rgb; w its hit distance
RWTexture2D<uint4> g_Light_Path : register(u15);            // x its bounce hit's cell (kGI1_InvalidId for none); y its direction (packNormal);
                                                            // zw the bounce's throughput (packHalf4)
RWStructuredBuffer<uint> g_HashGridCache_HashBuffer : register(u16);
RWStructuredBuffer<uint> g_HashGridCache_DecayTileBuffer : register(u17);
RWStructuredBuffer<uint2> g_HashGridCache_ValueBuffer : register(u18);
RWStructuredBuffer<uint2> g_HashGridCache_ValueIndirectBuffer : register(u19);
RWStructuredBuffer<uint> g_HashGridCache_UpdateTileBuffer : register(u20);
RWStructuredBuffer<uint> g_HashGridCache_UpdateCellValueBuffer : register(u21);
RWStructuredBuffer<uint> g_HashGridCache_UpdateCellValueIndirectBuffer : register(u22);
RWStructuredBuffer<float4> g_HashGridCache_VisibilityBuffer : register(u23);
RWStructuredBuffer<uint> g_HashGridCache_VisibilityCellBuffer : register(u24);
RWStructuredBuffer<uint> g_HashGridCache_VisibilityQueryBuffer : register(u25);
RWStructuredBuffer<uint2> g_HashGridCache_MultibounceInfoBuffer : register(u26); // The second hit's albedo
RWStructuredBuffer<uint> g_HashGridCache_PackedTileIndexBuffer : register(u27);
RWStructuredBuffer<uint> g_HashGridCache_PreviousPackedTileIndexBuffer : register(u28);
RWStructuredBuffer<uint> g_HashGridCache_LightBuffer : register(u29); // Hits to light: their visibility index

#define kGI1_InvalidId 0xFFFFFFFFu
#define kHashGridCache_TileDecay 50
#define kHashGridCache_FloatQuantize 1e3f
#define MAX_HIT_DISTANCE 1e9f
// The hit distance the denoiser gets for a ray that reaches the sky.
#define MISS_DISTANCE 1000.0f

// Capsaicin's names for this frame's constants.
#define g_BufferDimensions g_Frame.dimensions.xy
#define g_FrameIndex g_Frame.dimensions.z
#define g_Eye g_Frame.eye.xyz
#define g_NearFar g_Frame.nearFar.xy
#define g_ViewProjection g_Frame.viewProjection
#define g_PreviousViewProjection g_Frame.previousViewProjection
#define HASH_CELL_SIZE g_Frame.cellSizes.y
#define HASH_MIN_CELL_SIZE g_Frame.cellSizes.z
#define HASH_NUM_BUCKETS g_Frame.hashCounts.x
#define HASH_NUM_TILES_PER_BUCKET g_Frame.hashCounts.w
#define HASH_SIZE_TILE_MIP0 g_Frame.hashTileSizes.x
#define HASH_NUM_CELLS_PER_TILE_MIP0 g_Frame.hashTileCells.x
#define HASH_NUM_CELLS_PER_TILE g_Frame.hashMisc.x
#define HASH_TILE_CELL_RATIO g_Frame.hashSamples.w
#define HASH_MAX_SAMPLE_COUNT g_Frame.hashSamples.x
#define HASH_DISCARD_MULTIBOUNCE g_Frame.hashSamples.y
#define HASH_MAX_MULTIBOUNCE_SAMPLE_COUNT g_Frame.hashSamples.z
#define HASH_CELL_SAMPLES_PER_FRAME g_Frame.hashMisc.z
#define PACKED_TILE_COUNT (GI_COUNTER_PACKED_TILES + g_Frame.hashMisc.y)
#define PREVIOUS_PACKED_TILE_COUNT (GI_COUNTER_PACKED_TILES + 1 - g_Frame.hashMisc.y)

// ---- The game's frame and the light's grid ----

uint2 LightSize()
{
    return g_Frame.lightLayout.xy;
}

// The render pixel of its 2 x 2 a light pixel takes its sample from: the nearest of the four on light
// pixels of one checkerboard colour, the farthest on the other, as half-resolution effects downsample
// depth. With the top left, a wire, a fence or a grass blade under a pixel wide had it one frame and what
// lay behind the next as the upscaler's jitter moved them, and the denoiser started their light over
// every frame, the camera still (2026-10-25).
uint2 RenderPixelOf(uint2 lightPixel)
{
    uint2 corner = min(lightPixel * 2, g_BufferDimensions - 1);
    bool nearest = ((lightPixel.x + lightPixel.y) & 1u) == 0u;
    uint2 chosen = corner;
    float chosenDepth = g_Depth.Load(int3(corner, 0));
    for (uint i = 1; i < 4; ++i)
    {
        uint2 pixel = min(lightPixel * 2 + uint2(i & 1u, i >> 1), g_BufferDimensions - 1);
        float depth = g_Depth.Load(int3(pixel, 0));
        // Reversed Z: the nearer, the greater.
        if (nearest ? depth > chosenDepth : depth < chosenDepth)
        {
            chosen = pixel;
            chosenDepth = depth;
        }
    }

    return chosen;
}

float3 RemoveNaNs(float3 color)
{
    color /= (1.0f + color);
    color = saturate(color);
    return color / max(1.0f - color, 1e-4f);
}

// ---- Hash grid cache (Capsaicin's hash_grid_cache.hlsl) ----

// A hit kept for later passes, with the ray's direction, which turns the triangle's normal towards
// where the ray came from.
struct HashGridCache_Visibility
{
    float3 direction;
    uint instanceIndex;
    uint geometryIndex; // Within the instance's structure
    uint primitiveIndex;
    float2 barycentrics;
};

HashGridCache_Visibility VisibilityOf(SceneHit hit, float3 direction)
{
    HashGridCache_Visibility visibility;
    visibility.direction = direction;
    visibility.instanceIndex = hit.instance;
    visibility.geometryIndex = hit.geometry;
    visibility.primitiveIndex = hit.primitive;
    visibility.barycentrics = hit.barycentrics;
    return visibility;
}

#define HASHGRIDCACHE_STEP_FACTOR 1e3f
#define HASHGRIDCACHE_SIZE_FACTOR 1e-3f

struct HashGridCache_Desc
{
    uint bucketIndex;
    uint tileHash;
    uint2 cellOffset;
};

HashGridCache_Desc HashGridCache_GetDesc(float3 hitPosition, float3 direction, float hitDistance)
{
    float step = max(distance(g_Eye, hitPosition) * HASH_CELL_SIZE, HASH_MIN_CELL_SIZE);
    float logStep = floor(log2(HASHGRIDCACHE_STEP_FACTOR * step));
    float hitCellSize = HASHGRIDCACHE_SIZE_FACTOR * exp2(logStep);
    float hitTileSize = hitCellSize * HASH_TILE_CELL_RATIO;

    float3 signedC = floor(hitPosition / hitTileSize);
    float3 absDirection = abs(direction);
    float maxDirection = max(max(absDirection.x, absDirection.y), absDirection.z);
    uint axis = absDirection.x == maxDirection ? 0 : absDirection.y == maxDirection ? 1 : 2;

    uint l = uint(logStep);
    uint3 c = asuint(int3(signedC));
    uint3 d = uint3(axis, direction[axis] < 0.0f ? 1 : 0, 0);
    uint t = uint(hitDistance < hitTileSize);

    HashGridCache_Desc desc;
    desc.bucketIndex = pcgHash(l + pcgHash(c.x + pcgHash(c.y + pcgHash(c.z + pcgHash(d.x + pcgHash(d.y + pcgHash(d.z + pcgHash(t))))))))
        % HASH_NUM_BUCKETS;
    desc.tileHash = max(1, xxHash(l + xxHash(c.x + xxHash(c.y + xxHash(c.z + xxHash(d.x + xxHash(d.y + xxHash(d.z + xxHash(t)))))))));

    float3 e = floor(hitPosition / hitCellSize) - floor(hitPosition / hitTileSize) * HASH_TILE_CELL_RATIO;
    if (absDirection.x == maxDirection) desc.cellOffset = uint2(e.y, e.z);
    else if (absDirection.y == maxDirection) desc.cellOffset = uint2(e.x, e.z);
    else desc.cellOffset = uint2(e.x, e.y);

    return desc;
}

uint HashGridCache_CellOffsetMip0(uint cellIndexMip0, out uint2 cellOffsetMip0)
{
    uint tileIndex = cellIndexMip0 / HASH_NUM_CELLS_PER_TILE;
    uint linearOffset = cellIndexMip0 % HASH_NUM_CELLS_PER_TILE;
    cellOffsetMip0.y = linearOffset / HASH_SIZE_TILE_MIP0;
    cellOffsetMip0.x = linearOffset - cellOffsetMip0.y * HASH_SIZE_TILE_MIP0;
    return tileIndex;
}

uint HashGridCache_CellIndex(uint2 cellOffsetMip0, uint tileIndex, uint mip)
{
    uint mipSize = HASH_SIZE_TILE_MIP0 >> mip;
    uint2 cellOffset = cellOffsetMip0 >> mip;
    return tileIndex * HASH_NUM_CELLS_PER_TILE + g_Frame.hashTileFirstCells[mip] + cellOffset.x + cellOffset.y * mipSize;
}

uint HashGridCache_CellIndex(uint linearOffset, uint tileIndex)
{
    return tileIndex * HASH_NUM_CELLS_PER_TILE + linearOffset;
}

uint HashGridCache_InsertCell(float3 hitPosition, float3 direction, float hitDistance, out uint tileIndex, out bool isNewTile)
{
    uint bucketOffset;
    isNewTile = false;
    tileIndex = kGI1_InvalidId;
    HashGridCache_Desc desc = HashGridCache_GetDesc(hitPosition, direction, hitDistance);
    for (bucketOffset = 0; bucketOffset < HASH_NUM_TILES_PER_BUCKET; ++bucketOffset)
    {
        uint previousHash;
        tileIndex = bucketOffset + desc.bucketIndex * HASH_NUM_TILES_PER_BUCKET;
        InterlockedCompareExchange(g_HashGridCache_HashBuffer[tileIndex], 0, desc.tileHash, previousHash);
        if (previousHash == 0)
        {
            isNewTile = true;
            break;
        }

        if (previousHash == desc.tileHash) break;
    }

    if (bucketOffset >= HASH_NUM_TILES_PER_BUCKET) return kGI1_InvalidId;

    return HashGridCache_CellIndex(desc.cellOffset, tileIndex, 0);
}

uint4 HashGridCache_QuantizeRadiance(float3 radiance)
{
    return uint4(uint(round(kHashGridCache_FloatQuantize * radiance.x)), uint(round(kHashGridCache_FloatQuantize * radiance.y)),
        uint(round(kHashGridCache_FloatQuantize * radiance.z)), 1);
}

float4 HashGridCache_RecoverRadiance(uint4 quantized)
{
    return float4(quantized.xyz / kHashGridCache_FloatQuantize, quantized.w);
}

// The four sums from `first` (rgb quantised, and the samples), zeroed for the next frame's adds.
uint4 TakeQuantizedRadiance(RWStructuredBuffer<uint> sums, uint first)
{
    uint4 taken = uint4(sums[first + 0], sums[first + 1], sums[first + 2], sums[first + 3]);
    sums[first + 0] = 0;
    sums[first + 1] = 0;
    sums[first + 2] = 0;
    sums[first + 3] = 0;
    return taken;
}

float4 HashGridCache_PackVisibility(HashGridCache_Visibility visibility)
{
    return float4(asfloat(visibility.instanceIndex | (visibility.geometryIndex << GI_HIT_INSTANCE_BITS)), asfloat(packNormal(visibility.direction)),
        asfloat(visibility.primitiveIndex),
        asfloat((f32tof16(visibility.barycentrics.x) << 16) | f32tof16(visibility.barycentrics.y)));
}

HashGridCache_Visibility HashGridCache_UnpackVisibility(float4 packed)
{
    HashGridCache_Visibility visibility;
    visibility.instanceIndex = asuint(packed.x) & ((1u << GI_HIT_INSTANCE_BITS) - 1);
    visibility.geometryIndex = asuint(packed.x) >> GI_HIT_INSTANCE_BITS;
    visibility.direction = unpackNormal(asuint(packed.y));
    visibility.primitiveIndex = asuint(packed.z);
    visibility.barycentrics.x = f16tof32(asuint(packed.w) >> 16);
    visibility.barycentrics.y = f16tof32(asuint(packed.w) & 0xFFFFu);
    return visibility;
}

// The filtered radiance of a cell: the first mip of its tile with enough samples.
float4 HashGridCache_FilteredRadiance(RWStructuredBuffer<uint2> values, uint cellIndexMip0, float enough)
{
    uint2 offsetMip0;
    uint tileIndex = HashGridCache_CellOffsetMip0(cellIndexMip0, offsetMip0);
    float4 radiance = unpackHalf4(values[cellIndexMip0]);
    for (uint mip = 1; mip < 4; ++mip)
    {
        if (radiance.w >= enough || g_Frame.hashTileSizes[mip] == 0) break;

        radiance = unpackHalf4(values[HashGridCache_CellIndex(offsetMip0, tileIndex, mip)]);
    }

    return radiance;
}

float3 HashGridCache_FilteredDirect(uint cellIndex)
{
    float4 radiance = HashGridCache_FilteredRadiance(g_HashGridCache_ValueBuffer, cellIndex, HASH_MAX_SAMPLE_COUNT);
    return radiance.rgb / max(radiance.w, 1.0f);
}

float3 HashGridCache_FilteredIndirect(uint cellIndex)
{
    float4 radiance = HashGridCache_FilteredRadiance(g_HashGridCache_ValueIndirectBuffer, cellIndex, HASH_MAX_MULTIBOUNCE_SAMPLE_COUNT);
    return radiance.rgb / max(radiance.w, 1.0f);
}

// Claims a cell for this frame: new tiles join the packed list with their cells cleared, and the
// first touch of a tile this frame queues it for UpdateTiles.
void HashGridCache_TouchCell(uint tileIndex, bool isNewTile)
{
    uint previousDecay;
    InterlockedExchange(g_HashGridCache_DecayTileBuffer[tileIndex], g_FrameIndex, previousDecay);

    if (isNewTile)
    {
        uint packedTileIndex;
        InterlockedAdd(g_Counters[PACKED_TILE_COUNT], 1, packedTileIndex);
        g_HashGridCache_PackedTileIndexBuffer[packedTileIndex] = tileIndex;
        for (uint offset = 0; offset < HASH_NUM_CELLS_PER_TILE_MIP0; ++offset)
        {
            uint cell = HashGridCache_CellIndex(offset, tileIndex);
            g_HashGridCache_ValueBuffer[cell] = uint2(0, 0);
            g_HashGridCache_ValueIndirectBuffer[cell] = uint2(0, 0);
        }
    }

    if (isNewTile || previousDecay != g_FrameIndex)
    {
        uint updateTileIndex;
        InterlockedAdd(g_Counters[GI_COUNTER_UPDATE_TILES], 1, updateTileIndex);
        g_HashGridCache_UpdateTileBuffer[updateTileIndex] = tileIndex;
    }
}

// Queues a hit for PopulateCells as one of its cell's samples while the cell has fewer than
// HASH_CELL_SAMPLES_PER_FRAME this frame (the claims count in its update's w).
void HashGridCache_QueueLight(uint visibilityIndex, uint cellIndex)
{
    uint claimed;
    InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 3], 1, claimed);
    if (claimed >= HASH_CELL_SAMPLES_PER_FRAME) return;

    uint index;
    InterlockedAdd(g_Counters[GI_COUNTER_LIGHTS], 1, index);
    g_HashGridCache_LightBuffer[index] = visibilityIndex;
}

// A hit the cache keeps, for PopulateCells and the multibounce: `query` is where it came from.
uint HashGridCache_Keep(uint counter, uint first, HashGridCache_Visibility visibility, uint cellIndex, uint query)
{
    uint index;
    InterlockedAdd(g_Counters[counter], 1, index);
    uint visibilityIndex = first + index;
    g_HashGridCache_VisibilityBuffer[visibilityIndex] = HashGridCache_PackVisibility(visibility);
    g_HashGridCache_VisibilityCellBuffer[visibilityIndex] = cellIndex;
    g_HashGridCache_VisibilityQueryBuffer[visibilityIndex] = query;
    return index;
}

// ---- Hits ----

// World position and geometric normal (facing where the ray came from) of a stored hit.
void HitSurface(HashGridCache_Visibility visibility, out float3 world, out float3 normal)
{
    SceneTriangle t = LoadTriangle(visibility.instanceIndex, visibility.geometryIndex, visibility.primitiveIndex);
    world = TrianglePoint(t, visibility.barycentrics);
    normal = FacingNormal(t, visibility.direction);
}

// A hit's albedo: the G-buffer's where the camera sees the hit; otherwise its mesh's albedo texture,
// or its mesh's mean.
float3 HitAlbedo(float3 world, HashGridCache_Visibility visibility)
{
    float4 clip = mul(g_ViewProjection, float4(world, 1.0f));
    if (clip.w > 0.0f)
    {
        float2 ndc = clip.xy / clip.w;
        if (all(abs(ndc) < 1.0f))
        {
            uint2 pixel = min(uint2((ndc * float2(0.5f, -0.5f) + 0.5f) * g_BufferDimensions), g_BufferDimensions - 1);
            float depth = g_Depth.Load(int3(pixel, 0));
            if (depth > 0.0f && abs(toLinearDepth(depth, g_NearFar) - clip.w) < 0.02f * clip.w) return g_Albedo.Load(int3(pixel, 0)).rgb;
        }
    }

    // The texture's mean over a few texels' patch: the light varies over centimetres to metres.
    SceneTriangle t = LoadTriangle(visibility.instanceIndex, visibility.geometryIndex, visibility.primitiveIndex);
    if (t.geometry.texture && (t.geometry.flags & GI_GEOMETRY_MASK) == 0)
    {
        Texture2D<float4> texture = g_SceneTextures[NonUniformResourceIndex(t.geometry.texture - 1)];
        uint width, height, levels;
        texture.GetDimensions(0, width, height, levels);
        return texture.SampleLevel(g_WrapSampler, TriangleUV(t.geometry, t.indices, visibility.barycentrics), max(float(levels) - 6.0f, 0.0f)).rgb;
    }

    uint4 learnt = g_GeometryAlbedo[t.row];
    return learnt.w ? float3(learnt.xyz) / (255.0f * learnt.w) : g_Frame.nearFar.z;
}

// Leaves (GI_GEOMETRY_LEAF) let light through.
bool HitIsLeaf(HashGridCache_Visibility visibility)
{
    return (g_Geometries[FirstGeometry(g_Instances[visibility.instanceIndex]) + visibility.geometryIndex].flags & GI_GEOMETRY_LEAF) != 0;
}

// The light at a hit from one of the frame's point and spot lights, chosen by its light here without
// shadows (weighted reservoir sampling, a candidate a light), through a shadow ray to it: its light over
// the chance it was chosen.
float3 SampledLocalLight(float3 world, float3 normal, float3 origin, float tolerance, inout Random random)
{
    float total = 0.0f, chosenWeight = 0.0f;
    float3 chosenLight = 0.0f, chosenDirection = 0.0f;
    float chosenDistance = 0.0f, chosenClearance = 0.0f;
    for (uint i = 0; i < g_Frame.localLightCount.x; ++i)
    {
        GILocalLight candidate = g_Frame.localLights[i];
        float3 toLight;
        float lightDistance;
        float3 candidateLight = LocalLightAt(candidate, world, normal, toLight, lightDistance);
        float weight = luminance(candidateLight);
        if (!(weight > 0.0f)) continue;

        total += weight;
        if (random.rand() * total < weight)
        {
            chosenWeight = weight;
            chosenLight = candidateLight;
            chosenDirection = toLight;
            chosenDistance = lightDistance;
            chosenClearance = ClearanceTowards(candidate, toLight);
        }
    }

    if (!(chosenWeight > 0.0f)) return 0.0f;

    float rayLength = chosenDistance - chosenClearance;
    float visibility = rayLength > 0.0f ? VisibilityFromSurface(origin, normal, tolerance, chosenDirection, rayLength, ShadowFaces()) : 1.0f;
    return chosenLight * (total / chosenWeight * visibility);
}

// The share of the light on a leaf's far side it lets through to the near side, over the share it reflects
// (its albedo): a leaf is a thin surface that reflects and lets through light diffusely, as pbrt's
// DiffuseTransmissionMaterial. Green leaves let through a half to two thirds as much light as they reflect
// across the visible range (leaf optics measurements, PROSPECT). Without it a bush lit only by what its
// leaves reflected was black inside: the rays from inside it all met leaves within a metre (the shadow
// probe, 2026-10-24), each lit on its outer side alone. The sun's shadow rays still stop at a leaf: the
// light it lets through is scattered, which its glow here carries to what is around it.
static const float LeafTransmittance = 0.5f;

// The sun as a disc of its angular size at a hit: a shadow ray to a point on it from the side it lights,
// a leaf's far side too. Its light before the albedo.
float3 SunLight(float3 world, float3 normal, float3 origin, float tolerance, bool leaf, inout Random random)
{
    float3 towardsSun = g_Frame.sunDirection.xyz;
    bool sunFar = leaf && dot(normal, towardsSun) < 0.0f;
    float3 sunSide = sunFar ? -normal : normal;
    float cosine = dot(sunSide, towardsSun);
    if (!(cosine > 0.0f) || dot(g_Frame.sunColor.rgb, 1.0f) <= 0.0f) return 0.0f;

    float3 direction = SunSample(random.rand2());
    float3 sunOrigin = sunFar ? OffsetFromTriangle(world, sunSide) : origin;
    float visibility = VisibilityFromSurface(sunOrigin, sunSide, tolerance, direction, 1e4f, ShadowFaces());
    if (visibility <= 0.0f) return 0.0f;

    return g_Frame.sunColor.rgb * (cosine * visibility * (sunFar ? LeafTransmittance : 1.0f));
}

// The side a hit's cosine-weighted ray leaves from, and what its light counts: a leaf's ray goes to its
// far side in proportion to what that side gives, so either side's ray counts the same.
float3 BounceSide(float3 normal, bool leaf, inout Random random, out float weight)
{
    weight = leaf ? 1.0f + LeafTransmittance : 1.0f;
    return leaf && random.rand() * weight >= 1.0f ? -normal : normal;
}

// The light a cache cell's hit reflects from the sun, the sky and the frame's point and spot lights, in the
// game's diffuse light units times albedo; a leaf adds what it lets through from its far side.
float3 HitDirectLight(float3 world, float3 normal, float3 albedo, bool leaf, inout Random random)
{
    float3 origin = OffsetFromTriangle(world, normal);
    float tolerance = CopyTolerance(world);
    float3 light = SunLight(world, normal, origin, tolerance, leaf, random);

    // The sky through a cosine-weighted ray: its radiance is irradiance over pi for that sample.
    float skyWeight;
    float3 skySide = BounceSide(normal, leaf, random, skyWeight);
    float3 skyDirection = mapToCosineHemisphere(random.rand2(), skySide);
    float3 skyOrigin = all(skySide == normal) ? origin : OffsetFromTriangle(world, skySide);
    light += SkyRadiance(skyDirection) * (skyWeight * VisibilityFromSurface(skyOrigin, skySide, tolerance, skyDirection, 1e4f, false));

    light += SampledLocalLight(world, normal, origin, tolerance, random);
    return albedo * light;
}

// ---- Utilities ----

[numthreads(1, 1, 1)]
void ClearCounters()
{
    // The previous frame's packed tiles stay: PurgeTiles reads them this frame.
    g_Counters[PACKED_TILE_COUNT] = 0;
    for (uint counter = GI_COUNTER_UPDATE_TILES; counter < GI_COUNTER_COUNT; ++counter) g_Counters[counter] = 0;
}

// g_Dispatch: x counter, y threads (or tiles) a group handles, z argument slot, w a second counter to add.
[numthreads(1, 1, 1)]
void GenerateDispatch()
{
    uint count = g_Counters[g_Dispatch.x];
    if (g_Dispatch.w != GI_COUNTER_NONE) count += g_Counters[g_Dispatch.w];
    g_DispatchArgs[g_Dispatch.z] = uint4((count + g_Dispatch.y - 1) / g_Dispatch.y, 1, 1, 0);
}

// ---- The light's rays ----

// The upscaler's jitter (NDC) as a shift on screen, in uv.
float2 JitterUv(float2 ndc)
{
    return float2(0.5f, -0.5f) * ndc;
}

// The denoiser's guides for each light pixel: its view depth (the sky's beyond the denoising range), its
// normal and material (foliage's or not), and where it was on screen last frame, without the upscaler's
// jitter: the camera's motion, or a moving mesh's from the game's velocity, with the view depth's change by
// the camera's.
[numthreads(8, 8, 1)]
void PrepareGuides(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= LightSize())) return;

    uint2 pixel = RenderPixelOf(did);
    float depth = g_Depth.Load(int3(pixel, 0));
    float4 packedNormal = g_Normal.Load(int3(pixel, 0));
    float3 encoded = packedNormal.xyz;
    if (depth <= 0.0f || dot(encoded, encoded) == 0.0f)
    {
        g_Light_ViewZ[did] = GI_LIGHT_SKY_VIEW_Z;
        g_Light_NormalRoughness[did] = NRD_FrontEnd_PackNormalAndRoughness(float3(0.0f, 0.0f, 1.0f), 1.0f, 0.0f);
        g_Light_Motion[did] = 0.0f;
        return;
    }

    float viewZ = toLinearDepth(depth, g_NearFar);
    float2 uv = (pixel + 0.5f) / g_BufferDimensions;
    float4 previous = mul(g_PreviousViewProjection, float4(DepthPosition(pixel, depth), 1.0f));
    float2 previousUv = float2(0.5f, -0.5f) * previous.xy / previous.w + 0.5f;
    float2 moved;
    bool movingMesh = ObjectMotion(int2(pixel), moved);
    if (movingMesh) previousUv = uv - moved / g_BufferDimensions;

    // NRD takes the motion without the jitter, as its projections (denoiserCamera in gi_renderer.cpp). The
    // camera's motion here holds the jitter's change between the frames; the game's velocity runs from this
    // frame's jittered place to the last frame's unjittered one.
    float2 motion = previousUv - uv;
    motion += movingMesh ? JitterUv(g_Frame.jitter.xy) : JitterUv(g_Frame.jitter.xy - g_Frame.jitter.zw);

    // A moving mesh's view depth last frame: the last frame's where it was, when that lies within 10% of its own
    // now; else as if it stood still, which takes it as new where it was something else. As if it stood still
    // always, the change was off by the camera's movement where the camera follows it, as it does the player:
    // a few percent a frame, past NRD's 1%, and the player's light started over every frame (2026-10-25).
    float depthChange = previous.w - viewZ;
    if (movingMesh)
    {
        float2 lastUv = (did + 0.5f) / LightSize() + motion;
        if (all(lastUv > 0.0f) && all(lastUv < 1.0f))
        {
            float lastViewZ = g_Light_LastViewZ[min(uint2(lastUv * LightSize()), LightSize() - 1)];
            if (abs(lastViewZ - viewZ) < 0.1f * viewZ) depthChange = lastViewZ - viewZ;
        }
    }

    bool foliage = packedNormal.w < 0.5f;
    CountDiagnostic(GI_COUNTER_FOLIAGE, foliage);
    g_Light_ViewZ[did] = viewZ;
    g_Light_NormalRoughness[did] = NRD_FrontEnd_PackNormalAndRoughness(DecodeNormal(packedNormal), 1.0f, foliage ? GI_LIGHT_FOLIAGE_MATERIAL : 0.0f);
    g_Light_Motion[did] = float4(motion, depthChange, 0.0f);
}

// A light pixel's rays (the ray quality, 1, 2 or 4, twice on foliage): what each hits lit afresh, and its
// bounce, the sky's light or a cache cell's. The first ray's cell takes the hit's light and gives its own
// after the cache's update (ResolveLight); the others' cells give theirs as they stand, a frame old, and take none,
// the cache's buffers holding a hit a light pixel. Their light is averaged; their directions, weighed by
// it, give the denoiser's spherical harmonics (shorter than 1 where the rays' light spreads).
[numthreads(8, 8, 1)]
void TraceLight(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= LightSize())) return;

    uint2 pixel = RenderPixelOf(did);
    float depth = g_Depth.Load(int3(pixel, 0));
    float3 encoded = g_Normal.Load(int3(pixel, 0)).xyz;
    uint4 path = uint4(kGI1_InvalidId, 0, 0, 0);
    if (depth <= 0.0f || dot(encoded, encoded) == 0.0f)
    {
        g_Light_Partial[did] = 0.0f;
        g_Light_Path[did] = path;
        return;
    }

    float3 normal = DecodeNormal(float4(encoded, 0.0f));
    float3 world = DepthPosition(pixel, depth);
    Random random = MakeRandom(did.x + did.y * LightSize().x, g_FrameIndex);
    float3 origin = OffsetFromScreenSurface(world, normal);
    uint rays = clamp(g_Frame.lightRays.x, 1u, 4u);
    // Foliage (the normals' alpha under a half, PrepareGuides) takes twice: swaying and thin, it restarts its
    // denoiser history every few frames, so its light is mostly the frame's rays; shaded grass boiled (2026-10-25).
    if (g_Normal.Load(int3(pixel, 0)).w < 0.5f) rays *= 2u;
    float3 total = 0.0f, weightedDirection = 0.0f, firstDirection = 0.0f;
    float totalLuminance = 0.0f, totalDistance = 0.0f;
    for (uint ray = 0; ray < rays; ++ray)
    {
        float3 direction = mapToCosineHemisphere(random.rand2(), normal);
        if (ray == 0) firstDirection = direction;
        SceneHit hit = TraceFromSurface(origin, normal, CopyTolerance(world), direction, MAX_HIT_DISTANCE);
        CountDiagnostic(GI_COUNTER_HITS, hit.hit);
        CountDiagnostic(GI_COUNTER_NEAR_HITS, hit.hit && hit.distance < 0.25f);
        float3 light;
        float3 afterUpdate = 0.0f; // The first ray's cell's light as it stands, to weigh its direction
        if (!hit.hit)
        {
            light = SkyRadiance(direction);
            totalDistance += MISS_DISTANCE;
        }
        else
        {
            // The hit's own light, from the sun and a light.
            HashGridCache_Visibility visibility = VisibilityOf(hit, direction);
            float3 hitWorld, hitNormal;
            HitSurface(visibility, hitWorld, hitNormal);
            float3 albedo = HitAlbedo(hitWorld, visibility);
            bool leaf = HitIsLeaf(visibility);
            float3 hitOrigin = OffsetFromTriangle(hitWorld, hitNormal);
            float tolerance = CopyTolerance(hitWorld);
            light = albedo * (SunLight(hitWorld, hitNormal, hitOrigin, tolerance, leaf, random)
                + SampledLocalLight(hitWorld, hitNormal, hitOrigin, tolerance, random));
            MaxDiagnostic(GI_COUNTER_MAX_ALBEDO, albedo);
            MaxDiagnostic(GI_COUNTER_MAX_DIRECT, light);

            // Its bounce: the sky's light, or the cache's at what it reaches. The Lambert BRDF over the
            // cosine-weighted pdf is the albedo.
            float weight;
            float3 side = BounceSide(hitNormal, leaf, random, weight);
            float3 bounceDirection = mapToCosineHemisphere(random.rand2(), side);
            float3 bounceOrigin = all(side == hitNormal) ? hitOrigin : OffsetFromTriangle(hitWorld, side);
            SceneHit bounce = TraceFromSurface(bounceOrigin, side, tolerance, bounceDirection, MAX_HIT_DISTANCE);
            CountDiagnostic(GI_COUNTER_BOUNCE_HITS, bounce.hit);
            float3 throughput = albedo * weight;
            if (!bounce.hit) light += throughput * SkyRadiance(bounceDirection);
            else
            {
                float3 bounceHit = bounceOrigin + bounce.distance * bounceDirection;
                uint tileIndex;
                bool isNewTile;
                uint cellIndex = HashGridCache_InsertCell(bounceHit, bounceDirection, bounce.distance, tileIndex, isNewTile);
                CountDiagnostic(GI_COUNTER_FULL_BUCKETS, cellIndex == kGI1_InvalidId);
                if (cellIndex != kGI1_InvalidId)
                {
                    HashGridCache_TouchCell(tileIndex, isNewTile);
                    float3 cell = HashGridCache_FilteredDirect(cellIndex) + HashGridCache_FilteredIndirect(cellIndex);
                    if (ray == 0)
                    {
                        uint index = HashGridCache_Keep(GI_COUNTER_VISIBILITY0, 0, VisibilityOf(bounce, bounceDirection), cellIndex, did.x + did.y * LightSize().x);
                        HashGridCache_QueueLight(index, cellIndex);
                        path.x = cellIndex;
                        path.zw = packHalf4(float4(throughput / rays, 0.0f));
                        afterUpdate = throughput * cell;
                    }
                    else light += throughput * cell;
                }
            }

            totalDistance += hit.distance;
        }

        total += light;
        float rayLuminance = luminance(light + afterUpdate);
        weightedDirection += rayLuminance * direction;
        totalLuminance += rayLuminance;
    }

    path.y = packNormal(totalLuminance > 0.0f ? weightedDirection / totalLuminance : firstDirection);
    g_Light_Partial[did] = float4(total / rays, totalDistance / rays);
    g_Light_Path[did] = path;
}

// Teaches each mesh its mean albedo from the screen: a ray from the camera finds which mesh a pixel
// shows. One light pixel of each 16 x 16 a frame, moving over them; counts stop at 4096, so a mesh's
// albedo settles.
[numthreads(64, 1, 1)]
void LearnAlbedo(uint did : SV_DispatchThreadID)
{
    uint2 tiles = (LightSize() + 15) / 16;
    if (did >= tiles.x * tiles.y) return;

    uint2 jitter = min(uint2(CalculateHaltonSequence(g_FrameIndex) * 16.0f), 15u);
    uint2 lightPixel = uint2(did % tiles.x, did / tiles.x) * 16 + jitter;
    if (any(lightPixel >= LightSize())) return;

    uint2 seed = RenderPixelOf(lightPixel);
    float depth = g_Depth.Load(int3(seed, 0));
    if (depth <= 0.0f) return;

    float3 world = DepthPosition(seed, depth);
    float3 offset = world - g_Eye;
    float seedDistance = sqrt(dot(offset, offset));
    SceneHit hit = TraceClosest(g_Eye, offset / seedDistance, 1.05f * seedDistance + 0.5f);
    if (!hit.hit || abs(hit.distance - seedDistance) > 0.03f * seedDistance + 0.05f) return;

    uint row = FirstGeometry(g_Instances[hit.instance]) + hit.geometry;
    if (g_GeometryAlbedo[row].w >= 4096) return;

    uint3 albedo = uint3(round(saturate(g_Albedo.Load(int3(seed, 0)).rgb) * 255.0f));
    InterlockedAdd(g_GeometryAlbedo[row].x, albedo.x);
    InterlockedAdd(g_GeometryAlbedo[row].y, albedo.y);
    InterlockedAdd(g_GeometryAlbedo[row].z, albedo.z);
    InterlockedAdd(g_GeometryAlbedo[row].w, 1);
}

// ---- Hash grid cache kernels ----

[numthreads(64, 1, 1)]
void PurgeTiles(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[PREVIOUS_PACKED_TILE_COUNT]) return;

    // A tile's age from the frame index, which saves decrementing every tile each frame.
    uint tileIndex = g_HashGridCache_PreviousPackedTileIndexBuffer[did];
    uint tileDecay = g_HashGridCache_DecayTileBuffer[tileIndex];
    tileDecay = g_FrameIndex < tileDecay ? (0xFFFFFFFFu - tileDecay) + g_FrameIndex + 1 : g_FrameIndex - tileDecay;
    if (tileDecay >= kHashGridCache_TileDecay)
    {
        g_HashGridCache_HashBuffer[tileIndex] = 0;
        return;
    }

    uint packedTileIndex;
    InterlockedAdd(g_Counters[PACKED_TILE_COUNT], 1, packedTileIndex);
    g_HashGridCache_PackedTileIndexBuffer[packedTileIndex] = tileIndex;
}

// GI-1.2's multibounce: some second hits' cells trace on to a third hit's cell.
[numthreads(32, 1, 1)]
void PopulateMultibounceCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_VISIBILITY0]) return;

    Random random = MakeRandom(did, g_FrameIndex);
    if (random.rand() <= HASH_DISCARD_MULTIBOUNCE) return;

    HashGridCache_Visibility visibility = HashGridCache_UnpackVisibility(g_HashGridCache_VisibilityBuffer[did]);
    float3 world, normal;
    HitSurface(visibility, world, normal);
    float3 albedo = HitAlbedo(world, visibility);

    // Cosine-weighted: the Lambert BRDF over its pdf is the albedo.
    float3 direction = mapToCosineHemisphere(random.rand2(), normal);
    float3 origin = OffsetFromTriangle(world, normal);
    SceneHit hit = TraceFromSurface(origin, normal, CopyTolerance(world), direction, MAX_HIT_DISTANCE);
    if (!hit.hit) return;

    float3 hitPosition = origin + hit.distance * direction;
    uint tileIndex;
    bool isNewTile;
    uint cellIndex = HashGridCache_InsertCell(hitPosition, direction, hit.distance, tileIndex, isNewTile);
    if (cellIndex == kGI1_InvalidId) return;

    HashGridCache_TouchCell(tileIndex, isNewTile);
    // Third hits follow the second ones' room; Capsaicin divides by the chance to trace, but a cell
    // averages only the samples that were traced, which made its indirect light 1 / 0.3 too bright.
    uint index = HashGridCache_Keep(GI_COUNTER_VISIBILITY1, g_Frame.lightLayout.z, VisibilityOf(hit, direction), cellIndex, did);
    g_HashGridCache_MultibounceInfoBuffer[index] = packHalf4(float4(albedo, 0.0f));
    HashGridCache_QueueLight(g_Frame.lightLayout.z + index, cellIndex);
}

// The direct light of the queued hits, second and third, into their cells.
[numthreads(32, 1, 1)]
void PopulateCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_LIGHTS]) return;

    uint visibilityIndex = g_HashGridCache_LightBuffer[did];
    HashGridCache_Visibility visibility = HashGridCache_UnpackVisibility(g_HashGridCache_VisibilityBuffer[visibilityIndex]);
    float3 world, normal;
    HitSurface(visibility, world, normal);
    float3 albedo = HitAlbedo(world, visibility);
    Random random = MakeRandom(did + 0x9E3779B9u, g_FrameIndex);
    float3 lighting = HitDirectLight(world, normal, albedo, HitIsLeaf(visibility), random);
    MaxDiagnostic(GI_COUNTER_MAX_ALBEDO, albedo);
    MaxDiagnostic(GI_COUNTER_MAX_DIRECT, lighting);
    if (!(dot(lighting, lighting) > 0.0f)) return;

    // The sample's count went in with its claim (HashGridCache_QueueLight).
    uint cellIndex = g_HashGridCache_VisibilityCellBuffer[visibilityIndex];
    uint4 quantized = HashGridCache_QuantizeRadiance(RemoveNaNs(lighting));
    InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 0], quantized.x);
    InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 1], quantized.y);
    InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 2], quantized.z);
}

// Tiles of 8 x 8 cells: one 8 x 8 group a tile.
groupshared uint2 lds_UpdateTiles_ValueBuffer[8][8];
groupshared uint2 lds_UpdateTiles_ValueIndirectBuffer[8][8];

// `frameSamples`: the most samples the frame's update holds; a cell's direct light has more claims than samples.
float4 AccumulateCell(RWStructuredBuffer<uint2> values, RWStructuredBuffer<uint> updates, uint cellIndex, float maxSamples, uint frameSamples)
{
    float4 radiance = unpackHalf4(values[cellIndex]);
    uint4 sums = TakeQuantizedRadiance(updates, 4 * cellIndex);
    float4 added = HashGridCache_RecoverRadiance(uint4(sums.xyz, min(sums.w, frameSamples)));
    float sampleCount = min(radiance.w + added.w, maxSamples);
    radiance /= max(radiance.w, 1.0f);
    added /= max(added.w, 1.0f);
    radiance = radiance.w <= 0.0f ? added : lerp(radiance, added, 1.0f / sampleCount);
    // The sample count rides along as a hint for how far to filter.
    radiance *= sampleCount;
    return radiance;
}

[numthreads(8, 8, 1)]
void UpdateTiles(uint groupId : SV_GroupID, uint2 threadId : SV_GroupThreadID)
{
    // A group covers one tile of 8 x 8 cells (the tile cell ratio).
    if (groupId >= g_Counters[GI_COUNTER_UPDATE_TILES]) return;

    uint tileIndex = g_HashGridCache_UpdateTileBuffer[groupId];
    uint cellIndex = HashGridCache_CellIndex(threadId, tileIndex, 0);

    uint2 packedDirect = packHalf4(AccumulateCell(g_HashGridCache_ValueBuffer, g_HashGridCache_UpdateCellValueBuffer, cellIndex, HASH_MAX_SAMPLE_COUNT,
        HASH_CELL_SAMPLES_PER_FRAME));
    uint2 packedIndirect = packHalf4(AccumulateCell(g_HashGridCache_ValueIndirectBuffer, g_HashGridCache_UpdateCellValueIndirectBuffer, cellIndex,
        HASH_MAX_MULTIBOUNCE_SAMPLE_COUNT, 0xFFFFFFFFu));
    float4 storedDirect = unpackHalf4(packedDirect), storedIndirect = unpackHalf4(packedIndirect);
    MaxDiagnostic(GI_COUNTER_MAX_CELL, storedDirect.rgb / max(storedDirect.w, 1.0f));
    MaxDiagnostic(GI_COUNTER_MAX_INDIRECT, storedIndirect.rgb / max(storedIndirect.w, 1.0f));
    lds_UpdateTiles_ValueBuffer[threadId.x][threadId.y] = packedDirect;
    lds_UpdateTiles_ValueIndirectBuffer[threadId.x][threadId.y] = packedIndirect;
    g_HashGridCache_ValueBuffer[cellIndex] = packedDirect;
    g_HashGridCache_ValueIndirectBuffer[cellIndex] = packedIndirect;

    // Mips 1 to 3: box filters of the one below.
    [unroll]
    for (uint mip = 1; mip < 4; ++mip)
    {
        GroupMemoryBarrierWithGroupSync();
        uint step = 1u << (mip - 1);
        if (g_Frame.hashTileSizes[mip] > 0 && all((threadId % (step * 2)) == 0))
        {
            float4 direct = unpackHalf4(lds_UpdateTiles_ValueBuffer[threadId.x][threadId.y])
                + unpackHalf4(lds_UpdateTiles_ValueBuffer[threadId.x + step][threadId.y])
                + unpackHalf4(lds_UpdateTiles_ValueBuffer[threadId.x][threadId.y + step])
                + unpackHalf4(lds_UpdateTiles_ValueBuffer[threadId.x + step][threadId.y + step]);
            float4 indirect = unpackHalf4(lds_UpdateTiles_ValueIndirectBuffer[threadId.x][threadId.y])
                + unpackHalf4(lds_UpdateTiles_ValueIndirectBuffer[threadId.x + step][threadId.y])
                + unpackHalf4(lds_UpdateTiles_ValueIndirectBuffer[threadId.x][threadId.y + step])
                + unpackHalf4(lds_UpdateTiles_ValueIndirectBuffer[threadId.x + step][threadId.y + step]);
            uint mipCell = HashGridCache_CellIndex(threadId, tileIndex, mip);
            g_HashGridCache_ValueBuffer[mipCell] = packHalf4(direct);
            g_HashGridCache_ValueIndirectBuffer[mipCell] = packHalf4(indirect);
            lds_UpdateTiles_ValueBuffer[threadId.x][threadId.y] = packHalf4(direct);
            lds_UpdateTiles_ValueIndirectBuffer[threadId.x][threadId.y] = packHalf4(indirect);
        }
    }
}

// Third hits' cells' direct light, filtered, into their second hits' cells' indirect light; it takes
// effect next frame (GI-1.2).
[numthreads(32, 1, 1)]
void UpdateMultibounceCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_VISIBILITY1]) return;

    uint visibilityIndex = g_Frame.lightLayout.z + did;
    uint cellIndex = g_HashGridCache_VisibilityCellBuffer[visibilityIndex];
    uint baseCellIndex = g_HashGridCache_VisibilityCellBuffer[g_HashGridCache_VisibilityQueryBuffer[visibilityIndex]];
    float3 weight = unpackHalf4(g_HashGridCache_MultibounceInfoBuffer[did]).rgb;
    float3 radiance = HashGridCache_FilteredDirect(cellIndex) * weight;
    MaxDiagnostic(GI_COUNTER_MAX_BOUNCE, radiance);
    uint4 quantized = HashGridCache_QuantizeRadiance(RemoveNaNs(radiance));
    if (dot(radiance, radiance) > 0.0f)
    {
        InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 0], quantized.x);
        InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 1], quantized.y);
        InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 2], quantized.z);
    }
    InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 3], quantized.w);
}

// ---- Foliage's light by world position ----

// Grass and leaves sway, and the denoiser starts their light over every few frames, so shaded foliage boiled
// with each frame's rays (2026-10-25); a cell in the world stays where it is. Cells about 8 light pixels across where they lie (power-of-two sizes, 6.25 cm at the least), in
// a hash table of buckets (GI_FOLIAGE_MEMORY_SLOTS, GI_FOLIAGE_MEMORY_BUCKET). Each frame a foliage light pixel
// adds its light to its cell, found at a point jittered by up to half a cell each way (blending neighbouring
// cells over the frames), and takes FOLIAGE_MEMORY_SHARE of its light from the cell as the last frame left it,
// once the cell has 8 frames. UpdateFoliageMemory blends the frame's light into each cell over half a second
// (FOLIAGE_MEMORY_FRAMES) and frees the cells no foliage took for 2 seconds.
RWStructuredBuffer<uint> g_FoliageMemory_Keys : register(u31);    // A cell's key, 0 for a free slot
RWStructuredBuffer<uint> g_FoliageMemory_Touched : register(u32); // The frame foliage last took the cell
RWStructuredBuffer<uint> g_FoliageMemory_Added : register(u33);   // The frame's light into it, four a slot: rgb quantised
                                                                  // (HashGridCache_QuantizeRadiance), and the light pixels'
RWStructuredBuffer<uint2> g_FoliageMemory_Light : register(u34);  // Its light (packHalf4): rgb, and w its frames

#define FOLIAGE_MEMORY_SHARE 0.8f
#define FOLIAGE_MEMORY_FRAMES 30.0f
#define FOLIAGE_MEMORY_IDLE_FRAMES 120u

// The slot of the cell at `world`, found at a point `jitter` (-0.5 to 0.5 each way) cells off; kGI1_InvalidId
// when its bucket is full.
uint FoliageMemorySlot(float3 world, float3 jitter)
{
    float size = max(distance(g_Eye, world) * g_Frame.rayCone.x * 16.0f, 1.0f / 16.0f);
    int level = int(ceil(log2(size)));
    float cellSize = exp2(level);
    uint3 c = asuint(int3(floor(world / cellSize + jitter)));
    uint l = asuint(level);
    uint key = max(1u, xxHash(l + xxHash(c.x + xxHash(c.y + xxHash(c.z)))));
    uint bucket = pcgHash(l + pcgHash(c.x + pcgHash(c.y + pcgHash(c.z)))) % (GI_FOLIAGE_MEMORY_SLOTS / GI_FOLIAGE_MEMORY_BUCKET);
    for (uint i = 0; i < GI_FOLIAGE_MEMORY_BUCKET; ++i)
    {
        uint slot = bucket * GI_FOLIAGE_MEMORY_BUCKET + i;
        uint previous;
        InterlockedCompareExchange(g_FoliageMemory_Keys[slot], 0u, key, previous);
        if (previous == 0u || previous == key) return slot;
    }

    return kGI1_InvalidId;
}

// A light pixel's light with its cell's, if it shows foliage: adds its own to the cell's frame.
float3 RememberFoliageLight(uint2 lightPixel, float3 radiance)
{
    uint2 pixel = RenderPixelOf(lightPixel);
    float depth = g_Depth.Load(int3(pixel, 0));
    float4 packedNormal = g_Normal.Load(int3(pixel, 0));
    if (depth <= 0.0f || dot(packedNormal.xyz, packedNormal.xyz) == 0.0f || packedNormal.w >= 0.5f) return radiance;

    Random random = MakeRandom(lightPixel.x + lightPixel.y * LightSize().x + g_Frame.lightLayout.z, g_FrameIndex);
    float3 jitter = float3(random.rand2(), random.rand()) - 0.5f;
    uint slot = FoliageMemorySlot(DepthPosition(pixel, depth), jitter);
    CountDiagnostic(GI_COUNTER_FOLIAGE_MEMORY_FULL, slot == kGI1_InvalidId);
    if (slot == kGI1_InvalidId) return radiance;

    uint4 quantized = HashGridCache_QuantizeRadiance(RemoveNaNs(radiance));
    InterlockedAdd(g_FoliageMemory_Added[4 * slot + 0], quantized.x);
    InterlockedAdd(g_FoliageMemory_Added[4 * slot + 1], quantized.y);
    InterlockedAdd(g_FoliageMemory_Added[4 * slot + 2], quantized.z);
    InterlockedAdd(g_FoliageMemory_Added[4 * slot + 3], quantized.w);
    g_FoliageMemory_Touched[slot] = g_FrameIndex;
    float4 kept = unpackHalf4(g_FoliageMemory_Light[slot]);
    return lerp(radiance, kept.rgb, FOLIAGE_MEMORY_SHARE * saturate(kept.w / 8.0f));
}

[numthreads(64, 1, 1)]
void UpdateFoliageMemory(uint did : SV_DispatchThreadID)
{
    if (did >= GI_FOLIAGE_MEMORY_SLOTS || g_FoliageMemory_Keys[did] == 0u) return;

    if (g_FoliageMemory_Added[4 * did + 3] == 0u)
    {
        if (g_FrameIndex - g_FoliageMemory_Touched[did] > FOLIAGE_MEMORY_IDLE_FRAMES)
        {
            g_FoliageMemory_Keys[did] = 0u;
            g_FoliageMemory_Light[did] = uint2(0, 0);
        }
        return;
    }

    uint4 added = TakeQuantizedRadiance(g_FoliageMemory_Added, 4 * did);
    float4 kept = unpackHalf4(g_FoliageMemory_Light[did]);
    float frames = min(kept.w + 1.0f, FOLIAGE_MEMORY_FRAMES);
    float3 light = HashGridCache_RecoverRadiance(added).rgb / added.w;
    g_FoliageMemory_Light[did] = packHalf4(float4(lerp(kept.rgb, light, 1.0f / frames), frames));
}

// ---- Denoising and the render size ----

// Each ray's light with its bounce's cell, after the cache's update, and foliage's with its cell in the
// foliage memory, as the denoiser takes it.
[numthreads(8, 8, 1)]
void ResolveLight(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= LightSize())) return;

    float4 partial = g_Light_Partial[did];
    uint4 path = g_Light_Path[did];
    float3 radiance = partial.rgb;
    if (path.x != kGI1_InvalidId)
        radiance += unpackHalf4(path.zw).rgb * (HashGridCache_FilteredDirect(path.x) + HashGridCache_FilteredIndirect(path.x));

    MaxDiagnostic(GI_COUNTER_MAX_TRACED, radiance);
    radiance = RememberFoliageLight(did, radiance);
    // The rays' direction, at most 1 long (TraceLight).
    float3 direction = unpackNormal(path.y);
    float4 second;
    g_Light_Noisy[0][did] = RELAX_FrontEnd_PackSh(radiance, partial.w, direction * rsqrt(max(dot(direction, direction), 1.0f)), second, true);
    g_Light_Noisy[1][did] = second;
}

// A light pixel's denoised light at a surface of `normal`, its own normal being `ownNormal`: its spherical
// harmonics resolved against the normal (NRD_SH_ResolveDiffuse), scaled to keep the light it has at its own.
float3 DenoisedLight(uint2 lightPixel, float3 normal, float3 ownNormal)
{
    NRD_SG sg = RELAX_BackEnd_UnpackSh(g_Light_Denoised[0][lightPixel], g_Light_Denoised[1][lightPixel].xyz);
    float3 color = NRD_SG_ExtractColor(sg);
    float own = luminance(NRD_SH_ResolveDiffuse(sg, ownNormal));
    float here = luminance(NRD_SH_ResolveDiffuse(sg, normal));
    return own > 1e-6f ? color * clamp(here / own, 0.5f, 2.0f) : color;
}

// Diagnostics: the denoiser's history at each render pixel, blended over the image by its alpha
// (LightHistoryView, phantom_fx_settings.hpp): red where it started the light over within three frames, yellow under ten.
RWTexture2D<float4> g_LightDebug : register(u79);

float4 HistoryColor(float frames)
{
    if (frames < 3.0f) return float4(1.0f, 0.1f, 0.1f, 0.75f);
    if (frames < 10.0f) return float4(1.0f, 0.85f, 0.1f, 0.5f);
    return 0.0f;
}

// The light at every render pixel, irradiance over pi times the exposure: from the four light pixels
// around it, as a bilinear filter weighs them, times how alike their view depth and normal are to its own.
[numthreads(8, 8, 1)]
void CompositeLight(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_BufferDimensions)) return;

    float depth = g_Depth.Load(int3(did, 0));
    float3 encoded = g_Normal.Load(int3(did, 0)).xyz;
    if (depth <= 0.0f || dot(encoded, encoded) == 0.0f)
    {
        g_Output[did] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        if (g_Frame.lightDiagnostics.z != 0u) g_LightDebug[did] = 0.0f;
        return;
    }

    float3 normal = DecodeNormal(float4(encoded, 0.0f));
    float viewZ = toLinearDepth(depth, g_NearFar);
    // Light pixel p's ray left from render pixel 2p: an even render pixel is a light pixel, an odd one
    // halfway between two.
    float2 position = did * 0.5f;
    int2 base = int2(floor(position));
    float2 f = position - base;
    float3 total = 0.0f, nearest = 0.0f;
    float nearestHistory = 0.0f; // RELAX's history length in frames, with the light (OUT_DIFF_SH0.w)
    float weight = 0.0f, nearestDepth = FLT_MAX;
    for (uint i = 0; i < 4; ++i)
    {
        int2 offset = int2(i & 1, i >> 1);
        uint2 tap = uint2(clamp(base + offset, 0, int2(LightSize()) - 1));
        float tapViewZ = g_Light_ViewZ[tap];
        if (tapViewZ >= GI_LIGHT_DENOISING_RANGE) continue;

        float3 tapNormal = DecodeNormal(g_Normal.Load(int3(RenderPixelOf(tap), 0)));
        float3 light = DenoisedLight(tap, normal, tapNormal);
        MaxDiagnostic(GI_COUNTER_MAX_DENOISED, light);
        float depthDifference = abs(tapViewZ - viewZ) / viewZ;
        if (depthDifference < nearestDepth)
        {
            nearestDepth = depthDifference;
            nearest = light;
            nearestHistory = g_Light_Denoised[0][tap].w;
        }

        float bilinear = (offset.x ? f.x : 1.0f - f.x) * (offset.y ? f.y : 1.0f - f.y);
        float w = bilinear * saturate(1.0f - depthDifference / 0.05f) * pow(saturate(dot(tapNormal, normal)), 8.0f);
        total += w * light;
        weight += w;
    }

    // No light pixel of the same surface around (a thin edge): the one nearest in depth.
    float3 light = weight > 1e-4f ? total / weight : nearest;
    if (g_Frame.lightDiagnostics.z != 0u) g_LightDebug[did] = HistoryColor(nearestHistory);
    MaxDiagnostic(GI_COUNTER_MAX_OUTPUT, light);
    g_Output[did] = float4(light * g_Frame.nearFar.w, 1.0f);
}
