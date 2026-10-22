// GI-1.2's indirect light (docs/DEVELOPMENT.md, "Global illumination"): screen probes traced into
// a two-level world-space hash grid cache, ported from AMD Capsaicin's gi1.comp,
// screen_probes.hlsl, hash_grid_cache.hlsl and gi_denoiser.hlsl (MIT; licenses/Capsaicin-LICENSE.txt).
//
// Where it deviates from Capsaicin, and why:
// - The game has no materials the rays can reach, so a hit's albedo is the G-buffer's where the hit
//   is on screen and otherwise its mesh's mean albedo, learnt from the screen (LearnAlbedo).
// - The lights are the sun and the sky, as the game's own sky light gives them: each cell takes a
//   shadow ray to the sun and a cosine-weighted one to the sky (PopulateCells), in place of
//   Capsaicin's light sampler and reservoirs. Emissive surfaces and lamps are left out.
// - Positions come from the depth buffer, not a visibility buffer, and the motion from the camera.
// - The probes' spherical harmonics weigh each of a probe's 64 equal-area cells by its solid angle,
//   2 pi / 64; Capsaicin's 1 / 8 gave 4 / pi too much light.
// - Specular reflections (GI-1.1's glossy path) are left out: the game keeps its own.
// - Rays from a surface skip copies of it just above (gi_common.hlsli, "Copies of a surface").
// - A hash grid tile is keyed by the side of the surface a ray hits (the ray's main axis and its
//   sign), not by its finely quantised direction: diffuse light needs no more, and the grid fills
//   up far less (a hit without room gives no light).
// - Only the first few hits of a cell each frame bring it light (HashGridCache_QueueLight);
//   Capsaicin lights every hit, though dozens a frame land in most cells and a cell keeps 16 samples.
// - A probe cell no ray went to keeps its history, or else takes the mean of the traced cells;
//   Capsaicin gave it that mean over the number of empty cells, which darkened the probes frame by
//   frame (to 0.7 under a uniform sky) and which its 1 / 8 weight partly hid. A quarter of the rays
//   go to their own cell rather than where the last frame's light guides them, so every cell is
//   traced now and then.
#include "gi_common.hlsli"

// ---- Bindings, after gi_common.hlsli's ----

Texture2D<float> g_PreviousDepth : register(t7);
Texture2D<float4> g_PreviousNormal : register(t8);
Texture2D<uint> g_ScreenProbes_ProbeMask : register(t9); // The current mask with its mips, for lookups

RWTexture2D<float4> g_ScreenProbes_ProbeBuffer : register(u7);
RWTexture2D<float4> g_ScreenProbes_PreviousProbeBuffer : register(u8);
RWTexture2D<uint> g_ScreenProbes_ProbeMaskBuffer : register(u9);
RWTexture2D<uint> g_ScreenProbes_PreviousProbeMaskBuffer : register(u10);
RWStructuredBuffer<uint2> g_ScreenProbes_ProbeSHBuffer : register(u11);
RWStructuredBuffer<uint2> g_ScreenProbes_PreviousProbeSHBuffer : register(u12);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeSpawnBuffer : register(u13);
RWStructuredBuffer<uint> g_ScreenProbes_PreviousProbeSpawnBuffer : register(u14);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeSpawnScanBuffer : register(u15);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeSpawnIndexBuffer : register(u16);
RWStructuredBuffer<uint2> g_ScreenProbes_ProbeSpawnProbeBuffer : register(u17);
RWStructuredBuffer<uint2> g_ScreenProbes_ProbeSpawnSampleBuffer : register(u18);
RWStructuredBuffer<uint2> g_ScreenProbes_ProbeSpawnRadianceBuffer : register(u19);
RWStructuredBuffer<uint> g_ScreenProbes_EmptyTileBuffer : register(u20);
RWStructuredBuffer<uint> g_ScreenProbes_OverrideTileBuffer : register(u21);
RWTexture2D<float4> g_ScreenProbes_ProbeCachedTileBuffer : register(u22);
RWTexture2D<float4> g_ScreenProbes_ProbeCachedTileIndexBuffer : register(u23);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeCachedTileLRUBuffer : register(u24);
RWStructuredBuffer<uint> g_ScreenProbes_PreviousProbeCachedTileLRUBuffer : register(u25);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeCachedTileLRUFlagBuffer : register(u26);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeCachedTileLRUIndexBuffer : register(u27);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeCachedTileMRUBuffer : register(u28);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeCachedTileListBuffer : register(u29);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeCachedTileListCountBuffer : register(u30);
RWStructuredBuffer<uint> g_ScreenProbes_ProbeCachedTileListIndexBuffer : register(u31);
RWStructuredBuffer<uint4> g_ScreenProbes_ProbeCachedTileListElementBuffer : register(u32);
RWStructuredBuffer<uint> g_HashGridCache_HashBuffer : register(u33);
RWStructuredBuffer<uint> g_HashGridCache_DecayTileBuffer : register(u34);
RWStructuredBuffer<uint2> g_HashGridCache_ValueBuffer : register(u35);
RWStructuredBuffer<uint2> g_HashGridCache_ValueIndirectBuffer : register(u36);
RWStructuredBuffer<uint> g_HashGridCache_UpdateTileBuffer : register(u37);
RWStructuredBuffer<uint> g_HashGridCache_UpdateCellValueBuffer : register(u38);
RWStructuredBuffer<uint> g_HashGridCache_UpdateCellValueIndirectBuffer : register(u39);
RWStructuredBuffer<float4> g_HashGridCache_VisibilityBuffer : register(u40);
RWStructuredBuffer<uint> g_HashGridCache_VisibilityCellBuffer : register(u41);
RWStructuredBuffer<uint> g_HashGridCache_VisibilityQueryBuffer : register(u42);
RWStructuredBuffer<uint2> g_HashGridCache_VisibilityRadianceBuffer : register(u43); // A first-bounce hit's own light; w: 1 to bypass the cache
RWStructuredBuffer<uint2> g_HashGridCache_MultibounceInfoBuffer : register(u44);    // The first-bounce surface's albedo, over the chance to trace
RWStructuredBuffer<uint> g_HashGridCache_PackedTileIndexBuffer : register(u45);
RWStructuredBuffer<uint> g_HashGridCache_PreviousPackedTileIndexBuffer : register(u46);
RWTexture2D<float> g_GIDenoiser_BlurMask : register(u47);
RWTexture2D<float4> g_GIDenoiser_ColorBuffers[2] : register(u48);      // u48, u49: this frame's, the last frame's
RWTexture2D<float> g_GIDenoiser_ColorDeltaBuffers[2] : register(u50);  // u50, u51
RWTexture2D<float4> g_ScreenProbes_ProbeBufferPair[2] : register(u52); // u52, u53: this frame's probes, the last frame's
RWStructuredBuffer<uint> g_ScanInput : register(u54);
RWStructuredBuffer<uint> g_ScanOutput : register(u55);
RWStructuredBuffer<uint> g_ScanBlockSums : register(u56);
RWStructuredBuffer<uint> g_HashGridCache_LightBuffer : register(u57); // Hits to light: the visibility index, flags in the top bits
RWTexture2D<uint> g_ProbeMaskMips[12] : register(u64); // The current mask's mips, u64 to u75

#define kGI1_InvalidId 0xFFFFFFFFu
#define kGI1_AngleThreshold cos(2e-2f * PI)
#define kGI1_FloatQuantize 1e4f
#define kHashGridCache_TileDecay 50
#define kHashGridCache_FloatQuantize 1e3f
#define kGIDenoiser_MaxBlurMask 8.0f
#define MAX_HIT_DISTANCE 1e9f

// Capsaicin's names for this frame's constants.
#define g_BufferDimensions g_Frame.dimensions.xy
#define g_FrameIndex g_Frame.dimensions.z
#define g_Eye g_Frame.eye.xyz
#define g_NearFar g_Frame.nearFar.xy
#define g_ViewProjection g_Frame.viewProjection
#define g_ViewProjectionInverse g_Frame.viewProjectionInverse
#define g_PreviousViewProjection g_Frame.previousViewProjection
#define g_PreviousViewProjectionInverse g_Frame.previousViewProjectionInverse
#define PROBE_SIZE g_Frame.probeLayout.x
#define PROBE_SPAWN_TILE_SIZE g_Frame.probeLayout.y
#define PROBE_MASK_MIP_COUNT g_Frame.probeLimits.x
#define PROBE_CELL_SIZE g_Frame.cellSizes.x
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
#define LIGHT_SAMPLE 0x80000000u // The hit's light goes into its cell
#define LIGHT_OWN    0x40000000u // The hit keeps its own light, bypassing the cache
#define PACKED_TILE_COUNT (GI_COUNTER_PACKED_TILES + g_Frame.hashMisc.y)
#define PREVIOUS_PACKED_TILE_COUNT (GI_COUNTER_PACKED_TILES + 1 - g_Frame.hashMisc.y)

groupshared float lds_ScreenProbes_Radiance[64];
groupshared uint lds_ScreenProbes_Reprojection[4];
groupshared float4 lds_ScreenProbes_ProbeSHBuffer[9 * 64];
groupshared uint lds_ScreenProbes_RadianceValues[4 * 64];
groupshared uint lds_ScreenProbes_RadianceSampleCounts[64];
groupshared uint lds_ScreenProbes_RadianceReuseSampleCount;
groupshared float4 lds_ScreenProbes_RadianceBackup[64];

// ---- The game's frame ----

float3 DepthPosition(uint2 pixel, float depth)
{
    return transformPointProjection((pixel + 0.5f) / g_BufferDimensions, depth, g_ViewProjectionInverse);
}

// Screen motion of a pixel from the camera alone: its position now, where the camera saw it last frame.
float2 Velocity(uint2 pixel)
{
    float2 uv = (pixel + 0.5f) / g_BufferDimensions;
    float3 world = transformPointProjection(uv, g_Depth.Load(int3(pixel, 0)), g_ViewProjectionInverse);
    float3 previous = transformPointProjection(world, g_PreviousViewProjection);
    return uv - (0.5f * float2(previous.x, -previous.y) + 0.5f);
}

float GIDenoiser_RemoveNaNs(float color)
{
    color /= (1.0f + color);
    color = saturate(color);
    return color / max(1.0f - color, 1e-4f);
}

float3 GIDenoiser_RemoveNaNs(float3 color)
{
    color /= (1.0f + color);
    color = saturate(color);
    return color / max(1.0f - color, 1e-4f);
}

float4 GIDenoiser_RemoveNaNs(float4 color)
{
    color /= (1.0f + color);
    color = saturate(color);
    return color / max(1.0f - color, 1e-4f);
}

// ---- Screen probes (Capsaicin's screen_probes.hlsl) ----

uint ScreenProbes_PackSeed(uint2 seed)
{
    return (seed.x << 16) | seed.y;
}

uint2 ScreenProbes_UnpackSeed(uint packed)
{
    return uint2(packed >> 16, packed & 0xFFFFu);
}

uint ScreenProbes_FindClosestProbe(uint2 pos)
{
    uint2 dims = (g_BufferDimensions + PROBE_SIZE - 1) / PROBE_SIZE;
    pos = min(pos / PROBE_SIZE, dims - 1);
    for (uint i = 0; i < PROBE_MASK_MIP_COUNT; ++i)
    {
        uint probe = g_ScreenProbes_ProbeMask.Load(int3(pos, i)).x;
        if (probe != kGI1_InvalidId) return probe;

        dims = max(dims >> 1, 1);
        pos = min(pos >> 1, dims - 1);
    }

    return kGI1_InvalidId;
}

uint ScreenProbes_FindClosestProbe(uint2 pos, int2 offset)
{
    uint2 dims = (g_BufferDimensions + PROBE_SIZE - 1) / PROBE_SIZE;
    pos = min(pos / PROBE_SIZE, dims - 1);
    for (uint i = 0; i < PROBE_MASK_MIP_COUNT; ++i)
    {
        int2 loc = int2(pos) + offset;
        if (any(loc < 0) || any(loc >= int2(dims))) break;

        uint probe = g_ScreenProbes_ProbeMask.Load(int3(loc, i)).x;
        if (probe != kGI1_InvalidId) return probe;

        dims = max(dims >> 1, 1);
        pos = min(pos >> 1, dims - 1);
    }

    return kGI1_InvalidId;
}

float3 ScreenProbes_CalculateSHIrradiance(float3 normal, uint2 probe)
{
    float clampedCosine[9];
    SH_GetCoefficients_ClampedCosine_Cone(normal, HALF_PI, clampedCosine);

    uint probeCount = (g_BufferDimensions.x + PROBE_SIZE - 1) / PROBE_SIZE;
    uint probeIndex = probe.x + probe.y * probeCount;
    float3 irradiance = 0.0f;
    for (uint i = 0; i < 9; ++i) irradiance += clampedCosine[i] * unpackHalf4(g_ScreenProbes_ProbeSHBuffer[9 * probeIndex + i]).xyz;

    return max(irradiance, 0.0f);
}

uint4 ScreenProbes_QuantizeRadiance(float4 radiance)
{
    return uint4(round(kGI1_FloatQuantize * radiance));
}

float3 ScreenProbes_RecoverRadiance(uint3 quantized)
{
    return quantized / kGI1_FloatQuantize;
}

float4 ScreenProbes_RecoverRadiance(uint4 quantized)
{
    return quantized / kGI1_FloatQuantize;
}

void ScreenProbes_AccumulateRadiance(uint queryIndex, float3 radiance)
{
    if (dot(radiance, radiance) > 0.0f)
    {
        float4 radianceAndDistance = unpackHalf4(g_ScreenProbes_ProbeSpawnRadianceBuffer[queryIndex]);
        radianceAndDistance.xyz += radiance;
        g_ScreenProbes_ProbeSpawnRadianceBuffer[queryIndex] = packHalf4(radianceAndDistance);
    }
}

uint2 ScreenProbes_GetCellAndProbeIndex(uint queryIndex)
{
    return uint2(queryIndex % (PROBE_SIZE * PROBE_SIZE), queryIndex / (PROBE_SIZE * PROBE_SIZE));
}

uint ScreenProbes_FindCellIndex(uint localId, float s)
{
    uint index = 0;
    uint count = PROBE_SIZE * PROBE_SIZE;
    uint start = (localId / count) * count;
    while (0 < count)
    {
        uint count2 = count >> 1;
        uint middle = index + count2;
        if (lds_ScreenProbes_Radiance[middle + start] > s) count = count2;
        else
        {
            index = middle + 1;
            count -= count2 + 1;
        }
    }

    return max(index, 1) - 1;
}

// Returns the probe's total.
float ScreenProbes_ScanRadiance(uint localId, float radiance)
{
    lds_ScreenProbes_Radiance[localId] = radiance;
    GroupMemoryBarrierWithGroupSync();

    uint blockSize = PROBE_SIZE * PROBE_SIZE;
    uint firstLane = (localId / blockSize) * blockSize;
    uint localLane = localId - firstLane;

    radiance = lds_ScreenProbes_Radiance[firstLane + blockSize - 1];
    GroupMemoryBarrierWithGroupSync();

    uint stride;
    for (stride = 1; stride <= (blockSize >> 1); stride <<= 1)
    {
        if (localLane < blockSize / (2 * stride))
            lds_ScreenProbes_Radiance[firstLane + 2 * (localLane + 1) * stride - 1] += lds_ScreenProbes_Radiance[firstLane + (2 * localLane + 1) * stride - 1];
        GroupMemoryBarrierWithGroupSync();
    }

    if (localLane == 0) lds_ScreenProbes_Radiance[firstLane + blockSize - 1] = 0.0f;
    GroupMemoryBarrierWithGroupSync();

    for (stride = (blockSize >> 1); stride > 0; stride >>= 1)
    {
        if (localLane < blockSize / (2 * stride))
        {
            float temporary = lds_ScreenProbes_Radiance[firstLane + (2 * localLane + 1) * stride - 1];
            lds_ScreenProbes_Radiance[firstLane + (2 * localLane + 1) * stride - 1] = lds_ScreenProbes_Radiance[firstLane + 2 * (localLane + 1) * stride - 1];
            lds_ScreenProbes_Radiance[firstLane + 2 * (localLane + 1) * stride - 1] += temporary;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    radiance += lds_ScreenProbes_Radiance[firstLane + blockSize - 1];
    GroupMemoryBarrierWithGroupSync();

    lds_ScreenProbes_Radiance[localId] /= max(radiance, 1e-5f);
    GroupMemoryBarrierWithGroupSync();
    return radiance;
}

// A direction in a probe's hemisphere, from its cell (or a point within a cell).
float3 ProbeDirection(float2 cell, float3 normal)
{
    float3 direction = mapToHemiOctahedron(cell / PROBE_SIZE);
    float3 b1, b2;
    GetOrthoVectors(normal, b1, b2);
    return direction.x * b1 + direction.y * b2 + direction.z * normal;
}

uint ProbeCellOf(float3 direction, float3 normal)
{
    uint2 cell = uint2(mapToHemiOctahedronInverse(mul(direction, CreateTBN(normal))) * PROBE_SIZE);
    cell = min(cell, PROBE_SIZE - 1);
    return cell.x + cell.y * PROBE_SIZE;
}

// ---- Hash grid cache (Capsaicin's hash_grid_cache.hlsl) ----

// A hit kept for later passes. Capsaicin's geometry index is unused here; its place holds the
// ray's direction, which turns the triangle's normal towards where the ray came from.
struct HashGridCache_Visibility
{
    float3 direction;
    uint instanceIndex;
    uint primitiveIndex;
    float2 barycentrics;
};

#define HASHGRIDCACHE_STEP_FACTOR 1e3f
#define HASHGRIDCACHE_SIZE_FACTOR 1e-3f

float HashGridCache_GetCellSize(float3 position)
{
    float step = max(distance(g_Eye, position) * HASH_CELL_SIZE, HASH_MIN_CELL_SIZE);
    uint logStep = uint(log2(HASHGRIDCACHE_STEP_FACTOR * step));
    return HASHGRIDCACHE_SIZE_FACTOR * exp2(logStep);
}

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

float4 HashGridCache_PackVisibility(HashGridCache_Visibility visibility)
{
    return float4(asfloat(visibility.instanceIndex), asfloat(packNormal(visibility.direction)), asfloat(visibility.primitiveIndex),
        asfloat((f32tof16(visibility.barycentrics.x) << 16) | f32tof16(visibility.barycentrics.y)));
}

HashGridCache_Visibility HashGridCache_UnpackVisibility(float4 packed)
{
    HashGridCache_Visibility visibility;
    visibility.instanceIndex = asuint(packed.x);
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

// Queues a hit for PopulateCells: as one of its cell's samples while the cell has fewer than
// HASH_CELL_SAMPLES_PER_FRAME this frame (the claims count in its update's w), or for its own light.
void HashGridCache_QueueLight(uint visibilityIndex, uint cellIndex, bool own)
{
    uint claimed;
    InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 3], 1, claimed);
    bool sample = claimed < HASH_CELL_SAMPLES_PER_FRAME;
    if (!sample && !own) return;

    uint index;
    InterlockedAdd(g_Counters[GI_COUNTER_LIGHTS], 1, index);
    g_HashGridCache_LightBuffer[index] = visibilityIndex | (sample ? LIGHT_SAMPLE : 0u) | (own ? LIGHT_OWN : 0u);
}

// ---- Hits ----

// World position and geometric normal (facing where the ray came from) of a stored hit.
void HitSurface(HashGridCache_Visibility visibility, out float3 world, out float3 normal)
{
    GIInstanceRecord instance = g_InstanceRecords[visibility.instanceIndex];
    uint first = instance.vertexOffset + 3 * visibility.primitiveIndex;
    float3 v0 = g_Vertices[first].xyz, v1 = g_Vertices[first + 1].xyz, v2 = g_Vertices[first + 2].xyz;
    float2 b = visibility.barycentrics;
    world = (1.0f - b.x - b.y) * v0 + b.x * v1 + b.y * v2;
    normal = cross(v1 - v0, v2 - v0);
    float lengthSquared = dot(normal, normal);
    normal = lengthSquared > 0.0f ? normal * rsqrt(lengthSquared) : -visibility.direction;
    if (dot(normal, visibility.direction) > 0.0f) normal = -normal;
}

// A hit's albedo: the G-buffer's where the camera sees the hit, its mesh's mean otherwise.
float3 HitAlbedo(float3 world, uint instanceIndex)
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

    uint4 learnt = g_EntryAlbedo[g_InstanceRecords[instanceIndex].entry];
    return learnt.w ? float3(learnt.xyz) / (255.0f * learnt.w) : g_Frame.nearFar.z;
}

// The light a hit reflects from the sun and the sky, in the game's diffuse light units times albedo.
float3 HitDirectLight(float3 world, float3 normal, float3 albedo, inout Random random)
{
    float3 origin = OffsetFromTriangle(world, normal);
    float tolerance = CopyTolerance(world);
    float3 light = 0.0f;

    // The sun as a disc of its angular size: a shadow ray to a point on it.
    float3 towardsSun = g_Frame.sunDirection.xyz;
    float cosine = dot(normal, towardsSun);
    if (cosine > 0.0f && dot(g_Frame.sunColor.rgb, 1.0f) > 0.0f)
    {
        float3 b1, b2;
        GetOrthoVectors(towardsSun, b1, b2);
        float2 disc = random.rand2();
        float radius = sqrt(disc.x) * g_Frame.sunDirection.w;
        float angle = TWO_PI * disc.y;
        float3 direction = normalize(towardsSun + radius * (cos(angle) * b1 + sin(angle) * b2));
        if (VisibleFromSurface(origin, normal, tolerance, direction, 1e4f)) light += g_Frame.sunColor.rgb * cosine;
    }

    // The sky through a cosine-weighted ray: its radiance is irradiance over pi for that sample.
    float3 skyDirection = mapToCosineHemisphere(random.rand2(), normal);
    if (VisibleFromSurface(origin, normal, tolerance, skyDirection, 1e4f)) light += SkyRadiance(skyDirection);

    return albedo * light;
}

// ---- Utilities ----

[numthreads(1, 1, 1)]
void ClearCounters()
{
    g_Counters[GI_COUNTER_EMPTY_TILES] = 0;
    g_Counters[GI_COUNTER_OVERRIDE_TILES] = 0;
    g_Counters[GI_COUNTER_LRU] = 0;
    g_Counters[GI_COUNTER_MRU] = 0;
    g_Counters[GI_COUNTER_LIST_ELEMENTS] = 0;
    g_Counters[PACKED_TILE_COUNT] = 0;
    g_Counters[GI_COUNTER_UPDATE_TILES] = 0;
    g_Counters[GI_COUNTER_VISIBILITY0] = 0;
    g_Counters[GI_COUNTER_VISIBILITY1] = 0;
    g_Counters[GI_COUNTER_LIGHTS] = 0;
    g_Counters[GI_COUNTER_HITS] = 0;
    g_Counters[GI_COUNTER_NEAR_HITS] = 0;
    g_Counters[GI_COUNTER_COPIES] = 0;
    g_Counters[GI_COUNTER_FULL_BUCKETS] = 0;
}

// g_Dispatch: x counter, y threads (or tiles) a group handles, z argument slot, w a second counter to add.
[numthreads(1, 1, 1)]
void GenerateDispatch()
{
    uint count = g_Counters[g_Dispatch.x];
    if (g_Dispatch.w != GI_COUNTER_NONE) count += g_Counters[g_Dispatch.w];
    g_DispatchArgs[g_Dispatch.z] = uint4((count + g_Dispatch.y - 1) / g_Dispatch.y, 1, 1, 0);
}

// Fills g_ScanOutput, or a buffer chosen by g_Dispatch.y, with g_Dispatch.z; g_Dispatch.x elements.
[numthreads(64, 1, 1)]
void ClearBuffer(uint did : SV_DispatchThreadID)
{
    if (did >= g_Dispatch.x) return;

    if (g_Dispatch.y == 0) g_ScreenProbes_ProbeCachedTileListCountBuffer[did] = g_Dispatch.z;
    else if (g_Dispatch.y == 1) g_HashGridCache_HashBuffer[did] = g_Dispatch.z;
    else if (g_Dispatch.y == 2) g_HashGridCache_UpdateCellValueBuffer[did] = g_Dispatch.z;
    else if (g_Dispatch.y == 3) g_HashGridCache_UpdateCellValueIndirectBuffer[did] = g_Dispatch.z;
    else if (g_Dispatch.y == 4) g_HashGridCache_DecayTileBuffer[did] = g_Dispatch.z;
}

// Exclusive prefix sums of g_ScanInput's first g_Dispatch.x values into g_ScanOutput, in three
// passes: 1024 values a group, the groups' totals, then each group's offset.
groupshared uint lds_Scan[1024];

[numthreads(1024, 1, 1)]
void ScanBlocks(uint did : SV_DispatchThreadID, uint local : SV_GroupThreadID, uint group : SV_GroupID)
{
    uint value = did < g_Dispatch.x ? g_ScanInput[did] : 0;
    lds_Scan[local] = value;
    GroupMemoryBarrierWithGroupSync();

    for (uint offset = 1; offset < 1024; offset <<= 1)
    {
        uint add = local >= offset ? lds_Scan[local - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        lds_Scan[local] += add;
        GroupMemoryBarrierWithGroupSync();
    }

    if (did < g_Dispatch.x) g_ScanOutput[did] = lds_Scan[local] - value;
    if (local == 1023) g_ScanBlockSums[group] = lds_Scan[1023];
}

[numthreads(1024, 1, 1)]
void ScanBlockSums(uint local : SV_GroupThreadID)
{
    uint blocks = (g_Dispatch.x + 1023) / 1024;
    uint value = local < blocks ? g_ScanBlockSums[local] : 0;
    lds_Scan[local] = value;
    GroupMemoryBarrierWithGroupSync();

    for (uint offset = 1; offset < 1024; offset <<= 1)
    {
        uint add = local >= offset ? lds_Scan[local - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        lds_Scan[local] += add;
        GroupMemoryBarrierWithGroupSync();
    }

    if (local < blocks) g_ScanBlockSums[local] = lds_Scan[local] - value;
}

[numthreads(1024, 1, 1)]
void ScanAddOffsets(uint did : SV_DispatchThreadID, uint group : SV_GroupID)
{
    if (did < g_Dispatch.x) g_ScanOutput[did] += g_ScanBlockSums[group];
}

// ---- Screen probe kernels (Capsaicin's gi1.comp) ----

// g_Dispatch.x: the mip to clear.
[numthreads(8, 8, 1)]
void ClearProbeMask(uint2 did : SV_DispatchThreadID)
{
    uint2 dims;
    g_ProbeMaskMips[g_Dispatch.x].GetDimensions(dims.x, dims.y);
    if (all(did < dims)) g_ProbeMaskMips[g_Dispatch.x][did] = kGI1_InvalidId;
}

// g_Dispatch.x: the mip to fill from the one above it.
[numthreads(8, 8, 1)]
void FilterProbeMask(uint2 did : SV_DispatchThreadID)
{
    uint2 dims;
    g_ProbeMaskMips[g_Dispatch.x - 1].GetDimensions(dims.x, dims.y);
    if (any((did << 1) >= dims)) return;

    uint probeMask = kGI1_InvalidId;
    for (uint y = 0; y < 2 && probeMask == kGI1_InvalidId; ++y)
        for (uint x = 0; x < 2 && probeMask == kGI1_InvalidId; ++x)
        {
            uint2 pos = (did << 1) + uint2(x, y);
            probeMask = all(pos < dims) ? g_ProbeMaskMips[g_Dispatch.x - 1][pos] : kGI1_InvalidId;
        }

    g_ProbeMaskMips[g_Dispatch.x][did] = probeMask;
}

[numthreads(64, 1, 1)]
void InitCachedTileLRU(uint did : SV_DispatchThreadID)
{
    if (did < g_Frame.probeLayout.z * g_Frame.probeLayout.w) g_ScreenProbes_ProbeCachedTileLRUBuffer[did] = did | 0x80000000u;
}

[numthreads(8, 8, 1)]
void ReprojectScreenProbes(uint2 did : SV_DispatchThreadID, uint2 groupId : SV_GroupID, uint2 localId : SV_GroupThreadID, uint localIndex : SV_GroupIndex)
{
    uint2 pixel = min(did, g_BufferDimensions - 1);
    float depth = g_Depth.Load(int3(pixel, 0));
    float3 normal = all(did < g_BufferDimensions) ? g_Normal.Load(int3(did, 0)).xyz : float3(0.0f, 0.0f, 0.0f);
    bool isSkyPixel = dot(normal, normal) == 0.0f;
    normal = normalize(2.0f * normal - 1.0f);

    float2 uv = (did + 0.5f) / g_BufferDimensions;
    float3 worldPos = transformPointProjection(uv, depth, g_ViewProjectionInverse);
    float cellSize = distance(g_Eye, worldPos) * PROBE_CELL_SIZE;

    uint2 cell = localId % PROBE_SIZE;
    uint cellIndex = cell.x + cell.y * PROBE_SIZE;

    if (cellIndex == 0) lds_ScreenProbes_Reprojection[0] = (f32tof16(65504.0f) << 16) | 0xFFFFu;
    lds_ScreenProbes_RadianceValues[(localIndex << 2) + 0] = 0;
    lds_ScreenProbes_RadianceValues[(localIndex << 2) + 1] = 0;
    lds_ScreenProbes_RadianceValues[(localIndex << 2) + 2] = 0;
    lds_ScreenProbes_RadianceValues[(localIndex << 2) + 3] = 0;
    lds_ScreenProbes_RadianceSampleCounts[localIndex] = 0;
    GroupMemoryBarrierWithGroupSync();

    // The closest matching probe of the last frame: each of the 64 candidates scores, the best wins.
    if (!isSkyPixel && all(did < g_BufferDimensions))
    {
        float2 previousUv = uv - Velocity(did);
        int2 previousPos = int2(previousUv * g_BufferDimensions);
        if (all(previousPos >= 0) && all(previousPos < int2(g_BufferDimensions)))
        {
            uint probeMask = g_Frame.counts.w ? kGI1_InvalidId : g_ScreenProbes_PreviousProbeMaskBuffer[previousPos / PROBE_SIZE];
            if (probeMask != kGI1_InvalidId)
            {
                uint2 probeSeed = ScreenProbes_UnpackSeed(probeMask);
                float2 probeUv = (probeSeed + 0.5f) / g_BufferDimensions;
                float probeDepth = g_PreviousDepth.Load(int3(probeSeed, 0));
                float3 probeNormal = normalize(2.0f * g_PreviousNormal.Load(int3(probeSeed, 0)).xyz - 1.0f);
                float3 probePos = transformPointProjection(probeUv, probeDepth, g_PreviousViewProjectionInverse);
                if (abs(dot(probePos - worldPos, normal)) < cellSize && dot(normal, probeNormal) > 0.95f)
                {
                    uint probeScore = (f32tof16(distance(probePos, worldPos) / cellSize) << 16) | localIndex;
                    InterlockedMin(lds_ScreenProbes_Reprojection[0], probeScore);
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Each lane takes a cell of the chosen probe and reuses its radiance.
    uint localLane = lds_ScreenProbes_Reprojection[0] & 0xFFFFu;
    uint2 seed = (groupId << 3) + uint2(localLane & 7, localLane >> 3);
    uint2 safeSeed = min(seed, g_BufferDimensions - 1);
    uv = (seed + 0.5f) / g_BufferDimensions;
    float2 previousUv = uv - Velocity(safeSeed);
    uint2 previousPos = uint2(clamp(previousUv * g_BufferDimensions, 0.0f, g_BufferDimensions - 1.0f));
    uint probeMask = g_ScreenProbes_PreviousProbeMaskBuffer[previousPos / PROBE_SIZE];

    if (localLane != 0xFFFFu)
    {
        depth = g_Depth.Load(int3(seed, 0));
        normal = normalize(2.0f * g_Normal.Load(int3(seed, 0)).xyz - 1.0f);
        worldPos = transformPointProjection(uv, depth, g_ViewProjectionInverse);

        uint2 probeSeed = ScreenProbes_UnpackSeed(probeMask);
        float2 probeUv = (probeSeed + 0.5f) / g_BufferDimensions;
        float probeDepth = g_PreviousDepth.Load(int3(probeSeed, 0));
        float3 probePos = transformPointProjection(probeUv, probeDepth, g_PreviousViewProjectionInverse);
        float3 probeNormal = normalize(2.0f * g_PreviousNormal.Load(int3(probeSeed, 0)).xyz - 1.0f);
        float4 probeRadiance = g_ScreenProbes_PreviousProbeBuffer[(probeSeed / PROBE_SIZE) * PROBE_SIZE + cell];
        float3 probeDirection = ProbeDirection(cell + 0.5f, probeNormal);

        float3 hitPoint = probePos + probeDirection * probeRadiance.w;
        float3 reprojectedDirection = hitPoint - worldPos;
        float reprojectedLength = length(reprojectedDirection);
        reprojectedDirection /= reprojectedLength;
        if (dot(normal, reprojectedDirection) > 0.0f)
        {
            uint remapCellIndex = ProbeCellOf(reprojectedDirection, normal);
            uint4 remapRadiance = ScreenProbes_QuantizeRadiance(float4(probeRadiance.xyz, reprojectedLength));
            InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 0], remapRadiance.x);
            InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 1], remapRadiance.y);
            InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 2], remapRadiance.z);
            InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 3], remapRadiance.w);
            InterlockedAdd(lds_ScreenProbes_RadianceSampleCounts[remapCellIndex], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // The radiance for cells nothing landed in.
    lds_ScreenProbes_RadianceBackup[localIndex] = float4(ScreenProbes_RecoverRadiance(uint3(lds_ScreenProbes_RadianceValues[(localIndex << 2) + 0],
        lds_ScreenProbes_RadianceValues[(localIndex << 2) + 1], lds_ScreenProbes_RadianceValues[(localIndex << 2) + 2])),
        lds_ScreenProbes_RadianceSampleCounts[localIndex] > 0 ? 1.0f : 0.0f);
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 1; stride < 64; stride <<= 1)
    {
        if (localIndex < 64 / (2 * stride))
            lds_ScreenProbes_RadianceBackup[2 * (localIndex + 1) * stride - 1] += lds_ScreenProbes_RadianceBackup[(2 * localIndex + 1) * stride - 1];
        GroupMemoryBarrierWithGroupSync();
    }

    if (localIndex == 0)
    {
        float4 total = lds_ScreenProbes_RadianceBackup[63];
        lds_ScreenProbes_RadianceBackup[0] = float4(total.xyz / max(total.w, 1.0f), MAX_HIT_DISTANCE);
    }
    GroupMemoryBarrierWithGroupSync();

    // No probe: a disocclusion.
    if (localLane == 0xFFFFu)
    {
        if (cellIndex == 0)
        {
            uint2 probe = did / PROBE_SIZE;
            if (!isSkyPixel)
            {
                uint2 jitter = min(CalculateHaltonSequence(g_FrameIndex) * PROBE_SPAWN_TILE_SIZE, PROBE_SPAWN_TILE_SIZE - 1.0f);
                // A tile the spawn pass will not fill goes to the empty tiles, for adaptive sampling.
                if (any((jitter / PROBE_SIZE) != (probe % (PROBE_SPAWN_TILE_SIZE / PROBE_SIZE))))
                {
                    uint emptyTileIndex;
                    InterlockedAdd(g_Counters[GI_COUNTER_EMPTY_TILES], 1, emptyTileIndex);
                    g_ScreenProbes_EmptyTileBuffer[emptyTileIndex] = probe.x + probe.y * g_Frame.probeLayout.z;
                }
            }

            g_ScreenProbes_ProbeMaskBuffer[probe] = kGI1_InvalidId;
        }

        return;
    }

    if (cellIndex < 9)
    {
        uint2 probe = did / PROBE_SIZE;
        uint2 previousProbe = ScreenProbes_UnpackSeed(probeMask) / PROBE_SIZE;
        uint probeIndex = probe.x + probe.y * g_Frame.probeLayout.z;
        uint previousProbeIndex = previousProbe.x + previousProbe.y * g_Frame.probeLayout.z;
        if (cellIndex == 0) g_ScreenProbes_ProbeMaskBuffer[probe] = ScreenProbes_PackSeed(seed);
        g_ScreenProbes_ProbeSHBuffer[9 * probeIndex + cellIndex] = g_ScreenProbes_PreviousProbeSHBuffer[9 * previousProbeIndex + cellIndex];
    }

    float4 radiance = ScreenProbes_RecoverRadiance(uint4(lds_ScreenProbes_RadianceValues[(localIndex << 2) + 0],
        lds_ScreenProbes_RadianceValues[(localIndex << 2) + 1], lds_ScreenProbes_RadianceValues[(localIndex << 2) + 2],
        lds_ScreenProbes_RadianceValues[(localIndex << 2) + 3]));
    uint sampleCount = lds_ScreenProbes_RadianceSampleCounts[localIndex];
    if (sampleCount > 0) radiance /= sampleCount;
    else radiance = lds_ScreenProbes_RadianceBackup[0];

    g_ScreenProbes_ProbeBuffer[did] = radiance;
}

[numthreads(64, 1, 1)]
void CountScreenProbes(uint did : SV_DispatchThreadID)
{
    uint2 dims = g_Frame.probeLayout.zw;
    if (did >= dims.x * dims.y) return;

    uint cachedTileIndex = g_ScreenProbes_ProbeCachedTileLRUBuffer[did];
    if ((cachedTileIndex & 0x80000000u) == 0)
    {
        uint2 cachedProbe = uint2(cachedTileIndex % dims.x, cachedTileIndex / dims.x);
        float3 worldPos = g_ScreenProbes_ProbeCachedTileIndexBuffer[cachedProbe].xyz;
        float3 homogeneous = transformPointProjection(worldPos, g_ViewProjection);
        homogeneous.xy = 0.5f * float2(homogeneous.x, -homogeneous.y) + 0.5f;
        if (all(homogeneous > 0.0f) && all(homogeneous < 1.0f))
        {
            uint2 probe = uint2(homogeneous.xy * dims);
            uint probeIndex = probe.x + probe.y * dims.x;
            uint elementIndex;
            InterlockedAdd(g_Counters[GI_COUNTER_LIST_ELEMENTS], 1, elementIndex);
            uint indexInTile;
            InterlockedAdd(g_ScreenProbes_ProbeCachedTileListCountBuffer[probeIndex], 1, indexInTile);
            g_ScreenProbes_ProbeCachedTileListElementBuffer[elementIndex] = uint4(did, probeIndex, indexInTile, 0);
        }
    }

    g_ScreenProbes_ProbeCachedTileLRUFlagBuffer[did] = 1;
}

[numthreads(64, 1, 1)]
void ScatterScreenProbes(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_LIST_ELEMENTS]) return;

    uint3 element = g_ScreenProbes_ProbeCachedTileListElementBuffer[did].xyz;
    g_ScreenProbes_ProbeCachedTileListBuffer[g_ScreenProbes_ProbeCachedTileListIndexBuffer[element.y] + element.z] = element.x;
}

[numthreads(64, 1, 1)]
void SpawnScreenProbes(uint did : SV_DispatchThreadID)
{
    uint2 spawn = (g_BufferDimensions + PROBE_SPAWN_TILE_SIZE - 1) / PROBE_SPAWN_TILE_SIZE;
    if (did >= spawn.x * spawn.y) return;

    uint2 probe = uint2(did % spawn.x, did / spawn.x);
    uint2 jitter = min(CalculateHaltonSequence(g_FrameIndex) * PROBE_SPAWN_TILE_SIZE, PROBE_SPAWN_TILE_SIZE - 1.0f);
    uint2 seed = min(probe * PROBE_SPAWN_TILE_SIZE + jitter, g_BufferDimensions - 1);
    float3 normal = g_Normal.Load(int3(seed, 0)).xyz;
    bool isSkyPixel = dot(normal, normal) == 0.0f;
    if (!isSkyPixel) g_ScreenProbes_PreviousProbeSpawnBuffer[did] = ScreenProbes_PackSeed(seed);
    g_ScanInput[did] = isSkyPixel ? 0 : 1;
}

// After the scan: g_ScanInput holds whether each tile spawns, g_ScanOutput where it goes.
[numthreads(64, 1, 1)]
void CompactScreenProbes(uint did : SV_DispatchThreadID)
{
    if (did >= g_Frame.probeLimits.y || g_ScanInput[did] == 0) return;

    uint probeIndex = g_ScanOutput[did];
    uint probeSeed = g_ScreenProbes_PreviousProbeSpawnBuffer[did];
    uint probeMask = g_ScreenProbes_ProbeMaskBuffer[ScreenProbes_UnpackSeed(probeSeed) / PROBE_SIZE];
    // Not filling a hole from the reprojection: a tile an empty one may take the place of.
    if (probeMask != kGI1_InvalidId)
    {
        uint overrideTileIndex;
        InterlockedAdd(g_Counters[GI_COUNTER_OVERRIDE_TILES], 1, overrideTileIndex);
        g_ScreenProbes_OverrideTileBuffer[overrideTileIndex] = probeIndex;
    }

    g_ScreenProbes_ProbeSpawnBuffer[probeIndex] = probeSeed;
    g_ScreenProbes_ProbeSpawnIndexBuffer[did] = probeIndex;
}

// The number of probes that spawned, from the scan: the last tile's offset plus whether it spawned.
uint SpawnedProbeCount()
{
    uint last = g_Frame.probeLimits.y - 1;
    return g_ScanOutput[last] + g_ScanInput[last];
}

[numthreads(64, 1, 1)]
void PatchScreenProbes(uint did : SV_DispatchThreadID)
{
    uint overrideTileCount = g_Counters[GI_COUNTER_OVERRIDE_TILES];
    if (overrideTileCount == 0 || did >= g_Counters[GI_COUNTER_EMPTY_TILES]) return;

    uint probeIndex = g_ScreenProbes_EmptyTileBuffer[did];
    uint2 probe = uint2(probeIndex % g_Frame.probeLayout.z, probeIndex / g_Frame.probeLayout.z);
    uint2 jitter = min(CalculateHaltonSequence(g_FrameIndex) * PROBE_SIZE, PROBE_SIZE - 1.0f);
    uint2 seed = min(probe * PROBE_SIZE + jitter, g_BufferDimensions - 1);
    float3 normal = g_Normal.Load(int3(seed, 0)).xyz;
    if (dot(normal, normal) == 0.0f) return;

    Random random = MakeRandom(did, g_FrameIndex);
    uint index = random.randInt(overrideTileCount);
    uint previous;
    InterlockedExchange(g_ScreenProbes_ProbeSpawnBuffer[g_ScreenProbes_OverrideTileBuffer[index]], ScreenProbes_PackSeed(seed), previous);
}

// Keeps the spawn count where later passes read it: the scan buffers are reused.
[numthreads(1, 1, 1)]
void StoreSpawnCount()
{
    g_Counters[GI_COUNTER_COUNT - 1] = SpawnedProbeCount();
}

uint ProbeCount()
{
    return g_Counters[GI_COUNTER_COUNT - 1];
}

[numthreads(64, 1, 1)]
void SampleScreenProbes(uint did : SV_DispatchThreadID, uint localId : SV_GroupThreadID)
{
    uint probeCount = ProbeCount();
    uint2 cellAndProbe = ScreenProbes_GetCellAndProbeIndex(did);
    uint cellIndex = cellAndProbe.x;
    uint probeIndex = cellAndProbe.y;

    uint2 cell = uint2(cellIndex % PROBE_SIZE, cellIndex / PROBE_SIZE);
    uint2 seed = probeIndex < probeCount ? ScreenProbes_UnpackSeed(g_ScreenProbes_ProbeSpawnBuffer[probeIndex]) : uint2(0, 0);
    uint2 probe = seed / PROBE_SIZE;
    uint2 pos = probe * PROBE_SIZE + cell;

    float2 uv = (seed + 0.5f) / g_BufferDimensions;
    float depth = g_Depth.Load(int3(seed, 0));
    float3 normal = normalize(2.0f * g_Normal.Load(int3(seed, 0)).xyz - 1.0f);
    float3 worldPos = transformPointProjection(uv, depth, g_ViewProjectionInverse);

    uint previousProbeMask = kGI1_InvalidId;
    float cellSize = distance(g_Eye, worldPos) * PROBE_CELL_SIZE;
    int2 probeRes = int2(g_Frame.probeLayout.zw);
    uint2 dims = g_Frame.probeLayout.zw;

    if (localId == 0) lds_ScreenProbes_RadianceReuseSampleCount = 0;
    lds_ScreenProbes_RadianceValues[(localId << 2) + 0] = 0;
    lds_ScreenProbes_RadianceValues[(localId << 2) + 1] = 0;
    lds_ScreenProbes_RadianceValues[(localId << 2) + 2] = 0;
    lds_ScreenProbes_RadianceValues[(localId << 2) + 3] = 0;
    lds_ScreenProbes_RadianceSampleCounts[localId] = 0;
    GroupMemoryBarrierWithGroupSync();

    if (probeIndex < probeCount)
    {
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int2 tap = int2(probe) + int2(x, y);
                if (any(tap < 0) || any(tap >= probeRes)) continue;

                uint probeMask = g_ScreenProbes_ProbeMaskBuffer[tap];
                if (probeMask == kGI1_InvalidId) continue;
                if (x == 0 && y == 0) previousProbeMask = probeMask;

                uint2 probeSeed = ScreenProbes_UnpackSeed(probeMask);
                float3 probePos = DepthPosition(probeSeed, g_Depth.Load(int3(probeSeed, 0)));
                if (abs(dot(probePos - worldPos, normal)) > cellSize) continue;

                float3 probeNormal = normalize(2.0f * g_Normal.Load(int3(probeSeed, 0)).xyz - 1.0f);
                float4 probeRadiance = g_ScreenProbes_ProbeBuffer[(probeSeed / PROBE_SIZE) * PROBE_SIZE + cell];
                float3 hitPoint = probePos + ProbeDirection(cell + 0.5f, probeNormal) * probeRadiance.w;
                float3 reprojectedDirection = hitPoint - worldPos;
                float reprojectedLength = length(reprojectedDirection);
                reprojectedDirection /= reprojectedLength;
                if (dot(normal, reprojectedDirection) < 0.0f) continue;

                if (x == 0 && y == 0) InterlockedAdd(lds_ScreenProbes_RadianceReuseSampleCount, 1);

                uint remapCellIndex = ProbeCellOf(reprojectedDirection, normal);
                uint4 remapRadiance = ScreenProbes_QuantizeRadiance(float4(probeRadiance.xyz, reprojectedLength));
                InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 0], remapRadiance.x);
                InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 1], remapRadiance.y);
                InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 2], remapRadiance.z);
                InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 3], remapRadiance.w);
                InterlockedAdd(lds_ScreenProbes_RadianceSampleCounts[remapCellIndex], 1);
            }
    }
    GroupMemoryBarrierWithGroupSync();

    if (lds_ScreenProbes_RadianceReuseSampleCount > 0) previousProbeMask = kGI1_InvalidId;

    if (probeIndex < probeCount)
    {
        uint2 previousSeed = ScreenProbes_UnpackSeed(previousProbeMask);
        float3 previousProbePos = DepthPosition(previousSeed, g_Depth.Load(int3(min(previousSeed, g_BufferDimensions - 1), 0)));
        float3 previousProbeNormal = normalize(2.0f * g_Normal.Load(int3(min(previousSeed, g_BufferDimensions - 1), 0)).xyz - 1.0f);

        uint evictedProbeMask = kGI1_InvalidId, updatedProbeMask = kGI1_InvalidId;
        float bestEvictedScore = cellSize, bestUpdatedScore = cellSize;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int2 tap = int2(probe) + int2(x, y);
                if (any(tap < 0) || any(tap >= probeRes)) continue;

                uint tapIndex = uint(tap.x + tap.y * probeRes.x);
                uint cachedTileCount = g_ScreenProbes_ProbeCachedTileListCountBuffer[tapIndex];
                uint cachedTileStart = g_ScreenProbes_ProbeCachedTileListIndexBuffer[tapIndex];
                for (uint i = 0; i < cachedTileCount; ++i)
                {
                    uint cachedTile = g_ScreenProbes_ProbeCachedTileListBuffer[cachedTileStart + i];
                    uint cachedIndex = g_ScreenProbes_ProbeCachedTileLRUBuffer[cachedTile];
                    uint2 cachedProbe = uint2(cachedIndex % dims.x, cachedIndex / dims.x);
                    uint2 cachedPos = cachedProbe * PROBE_SIZE + cell;
                    float4 cachedProbePos = g_ScreenProbes_ProbeCachedTileIndexBuffer[cachedProbe];
                    float3 cachedProbeNormal = normalize(unpackNormal(asuint(cachedProbePos.w)));

                    if (abs(dot(cachedProbePos.xyz - worldPos, normal)) < cellSize)
                    {
                        float updatedScore = distance(cachedProbePos.xyz, worldPos);
                        if (updatedScore < bestUpdatedScore && dot(normal, cachedProbeNormal) > 0.95f)
                        {
                            updatedProbeMask = cachedTile;
                            bestUpdatedScore = updatedScore;
                        }

                        float4 cachedProbeRadiance = g_ScreenProbes_ProbeCachedTileBuffer[cachedPos];
                        float3 hitPoint = cachedProbePos.xyz + ProbeDirection(cell + 0.5f, cachedProbeNormal) * cachedProbeRadiance.w;
                        float3 reprojectedDirection = hitPoint - worldPos;
                        float reprojectedLength = length(reprojectedDirection);
                        reprojectedDirection /= reprojectedLength;
                        if (dot(normal, reprojectedDirection) > 0.0f)
                        {
                            uint remapCellIndex = ProbeCellOf(reprojectedDirection, normal);
                            uint4 remapRadiance = ScreenProbes_QuantizeRadiance(float4(cachedProbeRadiance.xyz, reprojectedLength));
                            InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 0], remapRadiance.x);
                            InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 1], remapRadiance.y);
                            InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 2], remapRadiance.z);
                            InterlockedAdd(lds_ScreenProbes_RadianceValues[(remapCellIndex << 2) + 3], remapRadiance.w);
                            InterlockedAdd(lds_ScreenProbes_RadianceSampleCounts[remapCellIndex], 1);
                        }
                    }

                    if (previousProbeMask != kGI1_InvalidId && abs(dot(cachedProbePos.xyz - previousProbePos, previousProbeNormal)) < cellSize)
                    {
                        float evictedScore = distance(cachedProbePos.xyz, previousProbePos);
                        if (evictedScore < bestEvictedScore && dot(previousProbeNormal, cachedProbeNormal) > 0.95f)
                        {
                            evictedProbeMask = cachedTile;
                            bestEvictedScore = evictedScore;
                        }
                    }
                }
            }

        if (cellIndex == 0)
        {
            if (previousProbeMask != kGI1_InvalidId)
            {
                if (evictedProbeMask == kGI1_InvalidId)
                {
                    InterlockedAdd(g_Counters[GI_COUNTER_LRU], 1, evictedProbeMask);
                    evictedProbeMask |= 0x80000000u; // A newly allocated cached probe
                }
                else
                {
                    uint flag;
                    InterlockedCompareExchange(g_ScreenProbes_ProbeCachedTileLRUFlagBuffer[evictedProbeMask], 1, 0, flag);
                    if (flag != 1) evictedProbeMask = kGI1_InvalidId;
                }
            }

            if (updatedProbeMask != kGI1_InvalidId)
            {
                uint flag;
                InterlockedCompareExchange(g_ScreenProbes_ProbeCachedTileLRUFlagBuffer[updatedProbeMask], 1, 0, flag);
                if (flag != 1) updatedProbeMask = kGI1_InvalidId;
            }

            g_ScreenProbes_ProbeSpawnProbeBuffer[probeIndex] = uint2(evictedProbeMask, updatedProbeMask);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    float totalWeight = float(lds_ScreenProbes_RadianceSampleCounts[localId]);
    float4 previousRadiance = ScreenProbes_RecoverRadiance(uint4(lds_ScreenProbes_RadianceValues[(localId << 2) + 0],
        lds_ScreenProbes_RadianceValues[(localId << 2) + 1], lds_ScreenProbes_RadianceValues[(localId << 2) + 2],
        lds_ScreenProbes_RadianceValues[(localId << 2) + 3]));
    if (totalWeight > 0.0f) previousRadiance /= totalWeight;
    else previousRadiance.w = -1.0f;

    float3 cellDirection = mapToHemiOctahedron((cell + 0.5f) / PROBE_SIZE);
    float guide = luminance(previousRadiance.xyz) * cellDirection.z;
    float guideTotal = ScreenProbes_ScanRadiance(localId, guide); // The sampling CDF

    if (probeIndex >= probeCount) return;

    // Diffuse only: ray guiding by last frame's radiance, for three rays in four.
    Random random = MakeRandom(did, g_FrameIndex);
    uint sampledCellIndex = ScreenProbes_FindCellIndex(localId, random.rand());
    bool guided = totalWeight > 0.0f && guideTotal > 0.0f && random.rand() >= 0.25f;
    uint2 sampledCell = guided ? uint2(sampledCellIndex % PROBE_SIZE, sampledCellIndex / PROBE_SIZE) : cell;
    float3 direction = ProbeDirection(sampledCell + random.rand2(), normal);

    g_ScreenProbes_PreviousProbeBuffer[pos] = previousRadiance;
    g_ScreenProbes_ProbeSpawnSampleBuffer[did] = packHalf3(direction);
}

// Teaches each mesh its mean albedo from the probes' pixels: a ray from the camera finds which
// mesh a pixel shows. Counts stop at 4096, so a mesh's albedo settles.
[numthreads(64, 1, 1)]
void LearnAlbedo(uint did : SV_DispatchThreadID)
{
    if (did >= ProbeCount()) return;

    uint2 seed = ScreenProbes_UnpackSeed(g_ScreenProbes_ProbeSpawnBuffer[did]);
    float depth = g_Depth.Load(int3(seed, 0));
    if (depth <= 0.0f) return;

    float3 world = DepthPosition(seed, depth);
    float3 offset = world - g_Eye;
    float seedDistance = sqrt(dot(offset, offset));
    SceneHit hit = TraceClosest(g_Eye, offset / seedDistance, 1.05f * seedDistance + 0.5f);
    if (!hit.hit || abs(hit.distance - seedDistance) > 0.03f * seedDistance + 0.05f) return;

    uint entry = g_InstanceRecords[hit.instance].entry;
    if (g_EntryAlbedo[entry].w >= 4096) return;

    uint3 albedo = uint3(round(saturate(g_Albedo.Load(int3(seed, 0)).rgb) * 255.0f));
    InterlockedAdd(g_EntryAlbedo[entry].x, albedo.x);
    InterlockedAdd(g_EntryAlbedo[entry].y, albedo.y);
    InterlockedAdd(g_EntryAlbedo[entry].z, albedo.z);
    InterlockedAdd(g_EntryAlbedo[entry].w, 1);
}

// One thread per probe ray: misses take the sky's radiance, hits become first-bounce cells.
[numthreads(32, 1, 1)]
void PopulateScreenProbes(uint did : SV_DispatchThreadID)
{
    uint2 cellAndProbe = ScreenProbes_GetCellAndProbeIndex(did);
    if (cellAndProbe.y >= ProbeCount()) return;

    uint2 seed = ScreenProbes_UnpackSeed(g_ScreenProbes_ProbeSpawnBuffer[cellAndProbe.y]);
    float3 encodedNormal = g_Normal.Load(int3(seed, 0)).xyz;
    if (dot(encodedNormal, encodedNormal) == 0.0f) return;

    float3 normal = normalize(2.0f * encodedNormal - 1.0f);
    float3 direction = unpackHalf3(g_ScreenProbes_ProbeSpawnSampleBuffer[did]);
    float3 world = DepthPosition(seed, g_Depth.Load(int3(seed, 0)));
    float3 origin = OffsetFromScreenSurface(world, normal);
    SceneHit hit = TraceFromSurface(origin, normal, CopyTolerance(world), direction, MAX_HIT_DISTANCE);

    float3 skySample = 0.0f;
    float hitDistance = MAX_HIT_DISTANCE;
    if (!hit.hit) skySample = SkyRadiance(direction);
    else
    {
        hitDistance = hit.distance;
        float3 hitPosition = origin + hit.distance * direction;
        uint tileIndex;
        bool isNewTile;
        uint cellIndex = HashGridCache_InsertCell(hitPosition, direction, hit.distance, tileIndex, isNewTile);
        CountDiagnostic(GI_COUNTER_FULL_BUCKETS, cellIndex == kGI1_InvalidId);
        if (cellIndex != kGI1_InvalidId)
        {
            HashGridCache_TouchCell(tileIndex, isNewTile);

            HashGridCache_Visibility visibility;
            visibility.direction = direction;
            visibility.instanceIndex = hit.instance;
            visibility.primitiveIndex = hit.primitive;
            visibility.barycentrics = hit.barycentrics;

            uint visibilityIndex;
            InterlockedAdd(g_Counters[GI_COUNTER_VISIBILITY0], 1, visibilityIndex);
            g_HashGridCache_VisibilityBuffer[visibilityIndex] = HashGridCache_PackVisibility(visibility);
            g_HashGridCache_VisibilityCellBuffer[visibilityIndex] = cellIndex;
            g_HashGridCache_VisibilityQueryBuffer[visibilityIndex] = did;

            // A ray shorter than a cell shares the cell with surfaces it should not see (a light
            // leak): it reads its own light instead of the cache, which PopulateCells writes.
            bool own = hit.distance < HashGridCache_GetCellSize(hitPosition);
            g_HashGridCache_VisibilityRadianceBuffer[visibilityIndex] = uint2(0, 0);
            HashGridCache_QueueLight(visibilityIndex, cellIndex, own);
        }
    }

    CountDiagnostic(GI_COUNTER_HITS, hit.hit);
    CountDiagnostic(GI_COUNTER_NEAR_HITS, hit.hit && hit.distance < 0.25f);
    g_ScreenProbes_ProbeSpawnRadianceBuffer[did] = packHalf4(float4(skySample, hitDistance));
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

// GI-1.2's multibounce: some first-bounce cells trace on to a second-bounce cell.
[numthreads(32, 1, 1)]
void PopulateMultibounceCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_VISIBILITY0]) return;

    Random random = MakeRandom(did, g_FrameIndex);
    if (random.rand() <= HASH_DISCARD_MULTIBOUNCE) return;

    HashGridCache_Visibility visibility = HashGridCache_UnpackVisibility(g_HashGridCache_VisibilityBuffer[did]);
    float3 world, normal;
    HitSurface(visibility, world, normal);
    float3 albedo = HitAlbedo(world, visibility.instanceIndex);

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

    HashGridCache_Visibility second;
    second.direction = direction;
    second.instanceIndex = hit.instance;
    second.primitiveIndex = hit.primitive;
    second.barycentrics = hit.barycentrics;

    uint index;
    InterlockedAdd(g_Counters[GI_COUNTER_VISIBILITY1], 1, index);
    // Capsaicin divides by the chance to trace, but a cell averages only the samples that were traced;
    // that made its indirect light 1 / 0.3 too bright.
    g_HashGridCache_MultibounceInfoBuffer[index] = packHalf4(float4(albedo, 0.0f));
    uint visibilityIndex = g_Frame.probeLimits.z + index; // Second-bounce hits follow the first-bounce ones' room
    g_HashGridCache_VisibilityBuffer[visibilityIndex] = HashGridCache_PackVisibility(second);
    g_HashGridCache_VisibilityCellBuffer[visibilityIndex] = cellIndex;
    g_HashGridCache_VisibilityQueryBuffer[visibilityIndex] = did; // The first-bounce hit it came from
    HashGridCache_QueueLight(visibilityIndex, cellIndex, false);
}

// The direct light of the queued hits, first and second bounce: into their cells, or as their own
// for first-bounce hits too close to their probe to read the cache (Capsaicin's bypass).
[numthreads(32, 1, 1)]
void PopulateCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_LIGHTS]) return;

    uint queued = g_HashGridCache_LightBuffer[did];
    uint visibilityIndex = queued & ~(LIGHT_SAMPLE | LIGHT_OWN);
    HashGridCache_Visibility visibility = HashGridCache_UnpackVisibility(g_HashGridCache_VisibilityBuffer[visibilityIndex]);
    float3 world, normal;
    HitSurface(visibility, world, normal);
    float3 albedo = HitAlbedo(world, visibility.instanceIndex);
    Random random = MakeRandom(did + 0x9E3779B9u, g_FrameIndex);
    float3 lighting = HitDirectLight(world, normal, albedo, random);

    // The sample's count went in with its claim (HashGridCache_QueueLight).
    uint cellIndex = g_HashGridCache_VisibilityCellBuffer[visibilityIndex];
    uint4 quantized = HashGridCache_QuantizeRadiance(lighting);
    if ((queued & LIGHT_SAMPLE) && dot(lighting, lighting) > 0.0f)
    {
        InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 0], quantized.x);
        InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 1], quantized.y);
        InterlockedAdd(g_HashGridCache_UpdateCellValueBuffer[4 * cellIndex + 2], quantized.z);
    }

    if (queued & LIGHT_OWN) g_HashGridCache_VisibilityRadianceBuffer[visibilityIndex] = packHalf4(float4(GIDenoiser_RemoveNaNs(lighting), 1.0f));
}

// Tiles of 8 x 8 cells: one 8 x 8 group a tile.
groupshared uint2 lds_UpdateTiles_ValueBuffer[8][8];
groupshared uint2 lds_UpdateTiles_ValueIndirectBuffer[8][8];

// `frameSamples`: the most samples the frame's update holds; a cell's direct light has more claims than samples.
float4 AccumulateCell(RWStructuredBuffer<uint2> values, RWStructuredBuffer<uint> updates, uint cellIndex, float maxSamples, uint frameSamples)
{
    float4 radiance = unpackHalf4(values[cellIndex]);
    float4 added = HashGridCache_RecoverRadiance(uint4(updates[4 * cellIndex + 0], updates[4 * cellIndex + 1], updates[4 * cellIndex + 2],
        min(updates[4 * cellIndex + 3], frameSamples)));
    float sampleCount = min(radiance.w + added.w, maxSamples);
    radiance /= max(radiance.w, 1.0f);
    added /= max(added.w, 1.0f);
    radiance = radiance.w <= 0.0f ? added : lerp(radiance, added, 1.0f / sampleCount);
    // The sample count rides along as a hint for how far to filter.
    radiance *= sampleCount;

    updates[4 * cellIndex + 0] = 0;
    updates[4 * cellIndex + 1] = 0;
    updates[4 * cellIndex + 2] = 0;
    updates[4 * cellIndex + 3] = 0;
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

// Second-bounce cells' direct light, filtered, into their first-bounce cells' indirect light; it
// takes effect next frame (GI-1.2).
[numthreads(32, 1, 1)]
void UpdateMultibounceCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_VISIBILITY1]) return;

    uint visibilityIndex = g_Frame.probeLimits.z + did;
    uint cellIndex = g_HashGridCache_VisibilityCellBuffer[visibilityIndex];
    uint baseCellIndex = g_HashGridCache_VisibilityCellBuffer[g_HashGridCache_VisibilityQueryBuffer[visibilityIndex]];
    float3 weight = unpackHalf4(g_HashGridCache_MultibounceInfoBuffer[did]).rgb;
    float3 radiance = HashGridCache_FilteredDirect(cellIndex) * weight;
    uint4 quantized = HashGridCache_QuantizeRadiance(GIDenoiser_RemoveNaNs(radiance));
    if (dot(radiance, radiance) > 0.0f)
    {
        InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 0], quantized.x);
        InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 1], quantized.y);
        InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 2], quantized.z);
    }
    InterlockedAdd(g_HashGridCache_UpdateCellValueIndirectBuffer[4 * baseCellIndex + 3], quantized.w);
}

// First-bounce cells, filtered, into the probe rays that found them.
[numthreads(64, 1, 1)]
void ResolveCells(uint did : SV_DispatchThreadID)
{
    if (did >= g_Counters[GI_COUNTER_VISIBILITY0]) return;

    uint cellIndex = g_HashGridCache_VisibilityCellBuffer[did];
    float4 own = unpackHalf4(g_HashGridCache_VisibilityRadianceBuffer[did]);
    float3 radiance = own.w > 0.0f ? own.rgb : HashGridCache_FilteredDirect(cellIndex);
    radiance += HashGridCache_FilteredIndirect(cellIndex);
    ScreenProbes_AccumulateRadiance(g_HashGridCache_VisibilityQueryBuffer[did], GIDenoiser_RemoveNaNs(radiance));
}

// ---- Screen probes, after the cache ----

[numthreads(64, 1, 1)]
void BlendScreenProbes(uint did : SV_DispatchThreadID, uint localId : SV_GroupThreadID)
{
    uint probeCount = ProbeCount();
    uint2 cellAndProbe = ScreenProbes_GetCellAndProbeIndex(did);
    uint cellIndex = cellAndProbe.x;
    uint probeIndex = cellAndProbe.y;
    uint2 cell = uint2(cellIndex % PROBE_SIZE, cellIndex / PROBE_SIZE);
    uint2 seed = probeIndex < probeCount ? ScreenProbes_UnpackSeed(g_ScreenProbes_ProbeSpawnBuffer[probeIndex]) : uint2(0, 0);
    uint2 probe = seed / PROBE_SIZE;
    uint2 pos = probe * PROBE_SIZE + cell;

    lds_ScreenProbes_RadianceValues[(localId << 2) + 0] = 0;
    lds_ScreenProbes_RadianceValues[(localId << 2) + 1] = 0;
    lds_ScreenProbes_RadianceValues[(localId << 2) + 2] = 0;
    lds_ScreenProbes_RadianceValues[(localId << 2) + 3] = 0;
    lds_ScreenProbes_RadianceSampleCounts[localId] = 0;
    GroupMemoryBarrierWithGroupSync();

    if (probeIndex < probeCount)
    {
        float3 normal = normalize(2.0f * g_Normal.Load(int3(seed, 0)).xyz - 1.0f);
        float3 direction = unpackHalf3(g_ScreenProbes_ProbeSpawnSampleBuffer[did]);
        float4 radiance = unpackHalf4(g_ScreenProbes_ProbeSpawnRadianceBuffer[did]);
        uint4 quantized = ScreenProbes_QuantizeRadiance(radiance);
        uint sampledCellIndex = ProbeCellOf(direction, normal);
        InterlockedAdd(lds_ScreenProbes_RadianceValues[(sampledCellIndex << 2) + 0], quantized.x);
        InterlockedAdd(lds_ScreenProbes_RadianceValues[(sampledCellIndex << 2) + 1], quantized.y);
        InterlockedAdd(lds_ScreenProbes_RadianceValues[(sampledCellIndex << 2) + 2], quantized.z);
        InterlockedAdd(lds_ScreenProbes_RadianceValues[(sampledCellIndex << 2) + 3], quantized.w);
        InterlockedAdd(lds_ScreenProbes_RadianceSampleCounts[sampledCellIndex], 1);
    }
    GroupMemoryBarrierWithGroupSync();

    // The radiance for cells no ray went to.
    lds_ScreenProbes_RadianceBackup[localId] = float4(ScreenProbes_RecoverRadiance(uint3(lds_ScreenProbes_RadianceValues[(localId << 2) + 0],
        lds_ScreenProbes_RadianceValues[(localId << 2) + 1], lds_ScreenProbes_RadianceValues[(localId << 2) + 2])),
        lds_ScreenProbes_RadianceSampleCounts[localId] > 0 ? 1.0f : 0.0f);
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 1; stride < 64; stride <<= 1)
    {
        if (localId < 64 / (2 * stride))
            lds_ScreenProbes_RadianceBackup[2 * (localId + 1) * stride - 1] += lds_ScreenProbes_RadianceBackup[(2 * localId + 1) * stride - 1];
        GroupMemoryBarrierWithGroupSync();
    }

    if (localId == 0)
    {
        float4 total = lds_ScreenProbes_RadianceBackup[63];
        lds_ScreenProbes_RadianceBackup[0] = float4(total.xyz / max(total.w, 1.0f), MAX_HIT_DISTANCE);
    }
    GroupMemoryBarrierWithGroupSync();

    if (probeIndex >= probeCount) return;

    float4 radiance = ScreenProbes_RecoverRadiance(uint4(lds_ScreenProbes_RadianceValues[(localId << 2) + 0],
        lds_ScreenProbes_RadianceValues[(localId << 2) + 1], lds_ScreenProbes_RadianceValues[(localId << 2) + 2],
        lds_ScreenProbes_RadianceValues[(localId << 2) + 3]));
    uint sampleCount = lds_ScreenProbes_RadianceSampleCounts[localId];
    float4 previousRadiance = g_ScreenProbes_PreviousProbeBuffer[pos];
    if (sampleCount > 0) radiance /= sampleCount;
    else radiance = previousRadiance.w > 0.0f ? previousRadiance : lds_ScreenProbes_RadianceBackup[0];

    if (previousRadiance.w > 0.0f)
    {
        // Shadow-preserving biased temporal hysteresis.
        float lumaA = luminance(radiance.xyz), lumaB = luminance(previousRadiance.xyz);
        float blend = squared(clamp(max(lumaA - lumaB - min(lumaA, lumaB), 0.0f) / max(max(lumaA, lumaB), 1e-4f), 0.0f, 0.95f));
        radiance = lerp(radiance, previousRadiance, blend);
    }

    uint2 dims = g_Frame.probeLayout.zw;
    uint2 probes = g_ScreenProbes_ProbeSpawnProbeBuffer[probeIndex];
    uint evictedProbeMask = probes.x, updatedProbeMask = probes.y;
    if (evictedProbeMask != kGI1_InvalidId && evictedProbeMask >= g_Counters[GI_COUNTER_LRU])
    {
        if ((evictedProbeMask & 0x80000000u) != 0)
        {
            evictedProbeMask &= ~0x80000000u;
            g_ScreenProbes_ProbeCachedTileLRUBuffer[evictedProbeMask] &= ~0x80000000u;
            g_ScreenProbes_ProbeCachedTileLRUFlagBuffer[evictedProbeMask] = 0;
        }

        uint cachedIndex = g_ScreenProbes_ProbeCachedTileLRUBuffer[evictedProbeMask];
        uint2 cachedProbe = uint2(cachedIndex % dims.x, cachedIndex / dims.x);
        uint2 cachedPos = cachedProbe * PROBE_SIZE + cell;
        if (cellIndex == 0)
        {
            uint previousProbeMask = g_ScreenProbes_ProbeMaskBuffer[probe];
            uint mruIndex;
            InterlockedAdd(g_Counters[GI_COUNTER_MRU], 1, mruIndex);
            g_ScreenProbes_ProbeCachedTileMRUBuffer[evictedProbeMask] = mruIndex;

            uint2 previousSeed = ScreenProbes_UnpackSeed(previousProbeMask);
            float3 previousWorld = DepthPosition(previousSeed, g_Depth.Load(int3(previousSeed, 0)));
            float3 previousNormal = normalize(2.0f * g_Normal.Load(int3(previousSeed, 0)).xyz - 1.0f);
            g_ScreenProbes_ProbeCachedTileIndexBuffer[cachedProbe] = float4(previousWorld, asfloat(packNormal(previousNormal)));
        }

        g_ScreenProbes_ProbeCachedTileBuffer[cachedPos] = g_ScreenProbes_ProbeBuffer[pos];
    }

    if (updatedProbeMask != kGI1_InvalidId && updatedProbeMask >= g_Counters[GI_COUNTER_LRU])
    {
        uint cachedIndex = g_ScreenProbes_ProbeCachedTileLRUBuffer[updatedProbeMask];
        uint2 cachedProbe = uint2(cachedIndex % dims.x, cachedIndex / dims.x);
        if (cellIndex == 0) g_ScreenProbes_ProbeCachedTileLRUFlagBuffer[updatedProbeMask] = 1;
        g_ScreenProbes_ProbeCachedTileBuffer[cachedProbe * PROBE_SIZE + cell] = radiance;
    }

    if (cellIndex == 0) g_ScreenProbes_ProbeMaskBuffer[probe] = ScreenProbes_PackSeed(seed);
    g_ScreenProbes_ProbeBuffer[pos] = radiance;
}

// Copies this frame's probes over the last frame's, which the filter reads (Capsaicin's copy).
[numthreads(8, 8, 1)]
void CopyProbes(uint2 did : SV_DispatchThreadID)
{
    uint2 dims = g_Frame.probeLayout.zw * PROBE_SIZE;
    if (all(did < dims)) g_ScreenProbes_PreviousProbeBuffer[did] = g_ScreenProbes_ProbeBuffer[did];
}

// After the scan: g_ScanInput holds the LRU flags, g_ScanOutput their offsets.
[numthreads(64, 1, 1)]
void ReorderScreenProbes(uint did : SV_DispatchThreadID)
{
    uint count = g_Frame.probeLayout.z * g_Frame.probeLayout.w;
    if (did >= count) return;

    uint cachedTileIndex = g_ScreenProbes_ProbeCachedTileLRUBuffer[did];
    uint cachedTileOffset = g_ScanOutput[did];
    if (g_ScanInput[did] == 0) cachedTileOffset = g_ScreenProbes_ProbeCachedTileMRUBuffer[did] + g_ScanOutput[count];
    g_ScreenProbes_PreviousProbeCachedTileLRUBuffer[cachedTileOffset] = cachedTileIndex;
}

// g_Dispatch: x the pair entry written, y the one read, zw the blur direction (0 or 1 each).
[numthreads(64, 1, 1)]
void FilterScreenProbes(uint did : SV_DispatchThreadID)
{
    uint2 cellAndProbe = ScreenProbes_GetCellAndProbeIndex(did);
    uint cellIndex = cellAndProbe.x;
    uint probeIndex = cellAndProbe.y;
    if (probeIndex >= ProbeCount()) return;

    int2 blurDirection = int2(g_Dispatch.zw);
    uint2 cell = uint2(cellIndex % PROBE_SIZE, cellIndex / PROBE_SIZE);
    uint2 seed = ScreenProbes_UnpackSeed(g_ScreenProbes_ProbeSpawnBuffer[probeIndex]);
    uint2 pos = (seed / PROBE_SIZE) * PROBE_SIZE + cell;

    float depth = g_Depth.Load(int3(seed, 0));
    float3 normal = normalize(2.0f * g_Normal.Load(int3(seed, 0)).xyz - 1.0f);
    float3 worldPos = DepthPosition(seed, depth);
    float cellSize = distance(g_Eye, worldPos) * PROBE_CELL_SIZE;

    float4 radiance = g_ScreenProbes_ProbeBufferPair[g_Dispatch.y][pos];
    float hitDistance = radiance.w;
    float totalWeight = 1.0f;
    float3 direction = ProbeDirection(cell + 0.5f, normal);

    for (int i = 0; i < 6; ++i)
    {
        int step = (((i & 1) << 1) - 1) * ((i >> 1) + 1);
        uint probeMask = ScreenProbes_FindClosestProbe(seed, step * blurDirection);
        if (probeMask == kGI1_InvalidId) continue;

        uint2 probeSeed = ScreenProbes_UnpackSeed(probeMask);
        float probeDepth = g_Depth.Load(int3(probeSeed, 0));
        float3 probeNormal = normalize(2.0f * g_Normal.Load(int3(probeSeed, 0)).xyz - 1.0f);
        float3 probeWorld = DepthPosition(probeSeed, probeDepth);
        if (abs(dot(probeWorld - worldPos, normal)) > cellSize || dot(direction, probeNormal) < 0.0f) continue;

        uint probeCellIndex = ProbeCellOf(direction, probeNormal);
        uint2 probeCell = uint2(probeCellIndex % PROBE_SIZE, probeCellIndex / PROBE_SIZE);
        uint2 probePos = (probeSeed / PROBE_SIZE) * PROBE_SIZE + probeCell;
        float3 probeDirection = normalize(ProbeDirection(probeCell + 0.5f, probeNormal));
        float4 probeRadiance = g_ScreenProbes_ProbeBufferPair[g_Dispatch.y][probePos];
        float probeHitDistance = min(probeRadiance.w, hitDistance);
        float3 hitPoint = probeWorld + probeDirection * probeHitDistance;
        if (dot(direction, normalize(hitPoint - worldPos)) < kGI1_AngleThreshold) continue;

        float weight = pow(saturate(1.0f - abs(toLinearDepth(probeDepth, g_NearFar) - toLinearDepth(depth, g_NearFar)) / toLinearDepth(depth, g_NearFar)), 8.0f);
        radiance += weight * float4(GIDenoiser_RemoveNaNs(probeRadiance.xyz), probeHitDistance);
        totalWeight += weight;
        hitDistance = radiance.w / totalWeight;
    }

    g_ScreenProbes_ProbeBufferPair[g_Dispatch.x][pos] = radiance / totalWeight;
}

[numthreads(64, 1, 1)]
void ProjectScreenProbes(uint did : SV_DispatchThreadID, uint localId : SV_GroupThreadID)
{
    uint probeCount = ProbeCount();
    uint2 cellAndProbe = ScreenProbes_GetCellAndProbeIndex(did);
    uint cellIndex = cellAndProbe.x;
    uint probeIndex = cellAndProbe.y;
    uint2 cell = uint2(cellIndex % PROBE_SIZE, cellIndex / PROBE_SIZE);
    uint2 seed = probeIndex < probeCount ? ScreenProbes_UnpackSeed(g_ScreenProbes_ProbeSpawnBuffer[probeIndex]) : uint2(0, 0);
    uint2 probe = seed / PROBE_SIZE;

    if (probeIndex < probeCount)
    {
        float3 normal = normalize(2.0f * g_Normal.Load(int3(seed, 0)).xyz - 1.0f);
        // Each of the 64 equal-area cells holds 2 pi / 64 of the hemisphere.
        float3 radiance = g_ScreenProbes_ProbeBuffer[probe * PROBE_SIZE + cell].xyz * (TWO_PI / (PROBE_SIZE * PROBE_SIZE));
        float coefficients[9];
        SH_GetCoefficients(ProbeDirection(cell + 0.5f, normal), coefficients);
        for (uint j = 0; j < 9; ++j) lds_ScreenProbes_ProbeSHBuffer[9 * localId + j] = float4(coefficients[j] * radiance, 1.0f);
    }
    GroupMemoryBarrierWithGroupSync();

    if (probeIndex >= probeCount || cellIndex >= 9) return;

    float4 irradianceSH = 0.0f;
    for (uint i = 0; i < 64; ++i) irradianceSH += lds_ScreenProbes_ProbeSHBuffer[9 * i + cellIndex];

    uint index = probe.x + probe.y * g_Frame.probeLayout.z;
    g_ScreenProbes_ProbeSHBuffer[9 * index + cellIndex] = packHalf4(irradianceSH);
}

// The probes' irradiance at every pixel; g_Dispatch.x is the colour buffer written.
[numthreads(8, 8, 1)]
void InterpolateScreenProbes(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_BufferDimensions)) return;

    float depth = g_Depth.Load(int3(did, 0));
    float3 encodedNormal = g_Normal.Load(int3(did, 0)).xyz;
    if (dot(encodedNormal, encodedNormal) == 0.0f)
    {
        g_GIDenoiser_ColorBuffers[g_Dispatch.x][did] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 normal = normalize(2.0f * encodedNormal - 1.0f);
    uint2 pos = did;
    float3 worldPos = DepthPosition(did, depth);
    float cellSize = distance(g_Eye, worldPos) * PROBE_CELL_SIZE;

    // A jitter within the pixel's plane spreads the probes' blocks.
    Random random = MakeRandom(did.x + did.y * g_BufferDimensions.x, g_FrameIndex);
    int2 jitter = int2((2.0f * random.rand2() - 1.0f) * PROBE_SPAWN_TILE_SIZE);
    uint2 newPos = clamp(int2(did) + jitter, 0, int2(g_BufferDimensions) - 1);
    float3 newWorldPos = DepthPosition(newPos, g_Depth.Load(int3(newPos, 0)));
    if (abs(dot(newWorldPos - worldPos, normal)) < 0.5f * cellSize) pos = newPos;

    uint4 probes;
    probes.x = ScreenProbes_FindClosestProbe(pos);
    if (probes.x == kGI1_InvalidId)
    {
        g_GIDenoiser_ColorBuffers[g_Dispatch.x][did] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    uint2 seed = ScreenProbes_UnpackSeed(probes.x);
    int2 offset = int2(pos.x < seed.x ? -1 : 1, pos.y < seed.y ? -1 : 1);
    probes.y = ScreenProbes_FindClosestProbe(pos, int2(offset.x, 0));
    probes.z = ScreenProbes_FindClosestProbe(pos, int2(0, offset.y));
    probes.w = ScreenProbes_FindClosestProbe(pos, offset);
    if (probes.y == probes.x) probes.y = kGI1_InvalidId;
    if (probes.z == probes.y || probes.z == probes.x) probes.z = kGI1_InvalidId;
    if (probes.w == probes.z || probes.w == probes.y || probes.w == probes.x) probes.w = kGI1_InvalidId;

    float4 w = 0.0f;
    for (uint i = 0; i < 4; ++i)
    {
        if (probes[i] == kGI1_InvalidId) continue;

        uint2 probeSeed = ScreenProbes_UnpackSeed(probes[i]);
        float probeDepth = g_Depth.Load(int3(probeSeed, 0));
        float3 probePos = DepthPosition(probeSeed, probeDepth);
        if (abs(dot(probePos - worldPos, normal)) > cellSize) w[i] = 0.0f; // Probes ahead of the pixel's plane would leak into the background
        else
        {
            w[i] = saturate(1.0f - abs(toLinearDepth(probeDepth, g_NearFar) - toLinearDepth(depth, g_NearFar)) / toLinearDepth(depth, g_NearFar));
            w[i] *= max(dot(normal, normalize(2.0f * g_Normal.Load(int3(probeSeed, 0)).xyz - 1.0f)), 0.0f);
            w[i] = pow(w[i], 8.0f);
        }
    }

    bool useBackup = false;
    if (dot(w, w) == 0.0f)
    {
        w = float4(1.0f, probes.y != kGI1_InvalidId ? 1.0f : 0.0f, probes.z != kGI1_InvalidId ? 1.0f : 0.0f, probes.w != kGI1_InvalidId ? 1.0f : 0.0f);
        useBackup = true;
    }

    w /= w.x + w.y + w.z + w.w;

    float3 irradiance = 0.0f;
    for (uint j = 0; j < 4; ++j)
        if (w[j] > 0.0f) irradiance += w[j] * ScreenProbes_CalculateSHIrradiance(normal, ScreenProbes_UnpackSeed(probes[j]) / PROBE_SIZE);

    g_GIDenoiser_ColorBuffers[g_Dispatch.x][did] = float4(irradiance, useBackup ? 0.0f : 1.0f);
}

// ---- Denoiser (Capsaicin's gi_denoiser.hlsl) ----

int GIDenoiser_GetBlurRadius(uint2 pos)
{
    float blurMask = g_GIDenoiser_BlurMask[pos] * kGIDenoiser_MaxBlurMask;
    return blurMask > 0.0f ? int(max(blurMask, 1.0f) + 0.5f) : 0;
}

// g_Dispatch: x this frame's colour buffer, y the last frame's.
[numthreads(8, 8, 1)]
void ReprojectGI(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_BufferDimensions)) return;

    float4 color = g_GIDenoiser_ColorBuffers[g_Dispatch.x][did];
    float4 lighting = 0.0f;
    float3 normal = g_Normal.Load(int3(did, 0)).xyz;
    float alphaBlend = 1.0f;
    float colorDelta = 0.0f;
    float2 uv = (did + 0.5f) / g_BufferDimensions;
    bool isSkyPixel = dot(normal, normal) == 0.0f;

    if (!isSkyPixel)
    {
        float2 previousUv = uv - Velocity(did);
        float depth = g_Depth.Load(int3(did, 0));
        normal = 2.0f * normal - 1.0f;
        if (all(previousUv > 0.0f) && all(previousUv < 1.0f))
        {
            float3 world = transformPointProjection(uv, depth, g_ViewProjectionInverse);
            float cellSize = distance(g_Eye, world) * PROBE_CELL_SIZE;
            cellSize *= lerp(1.0f, 5.0f, pow(1.0f - max(dot(normalize(g_Eye - world), normalize(normal)), 0.0f), 6.0f));

            float weight = 0.0f;
            for (float y = -1.0f; y <= 1.0f; ++y)
                for (float x = -1.0f; x <= 1.0f; ++x)
                {
                    int2 tap = int2(floor(previousUv * g_BufferDimensions + float2(x, y)));
                    if (any(tap < 0) || any(tap >= int2(g_BufferDimensions))) continue;

                    float4 c = g_GIDenoiser_ColorBuffers[g_Dispatch.y][tap];
                    float3 previousNormal = g_PreviousNormal.Load(int3(tap, 0)).xyz;
                    if (c.w < 1.0f || dot(previousNormal, previousNormal) == 0.0f) continue;

                    previousNormal = 2.0f * previousNormal - 1.0f;
                    float2 tapUv = (tap + 0.5f) / g_BufferDimensions;
                    float3 previousWorld = transformPointProjection(tapUv, g_PreviousDepth.Load(int3(tap, 0)), g_PreviousViewProjectionInverse);
                    if (distance(world, previousWorld) < cellSize && dot(normal, previousNormal) > 0.95f)
                    {
                        float subpixel = distance(tap + 0.5f, previousUv * g_BufferDimensions);
                        float w = saturate(1.0f - subpixel * 0.707107f);
                        colorDelta += w * g_GIDenoiser_ColorDeltaBuffers[g_Dispatch.y][tap];
                        lighting += w * c;
                        weight += w;
                    }
                }

            if (weight > 0.0f)
            {
                float sign = colorDelta < 0.0f ? -1.0f : 1.0f;
                colorDelta = sign * GIDenoiser_RemoveNaNs(abs(colorDelta) / weight);
                lighting = GIDenoiser_RemoveNaNs(lighting / weight);
            }

            if (color.w > 0.0f)
            {
                float lumaA = luminance(color.xyz);
                float lumaB = luminance(lighting.xyz / max(lighting.w, 1.0f));
                colorDelta = lerp(colorDelta, lumaA - lumaB, 1.0f / 8.0f);
                alphaBlend = saturate(1.0f - abs(colorDelta) / max(lumaB, 1e-4f));
            }
        }
    }

    float blurMask = !isSkyPixel ? max(kGIDenoiser_MaxBlurMask - lighting.w, 0.0f) : -1.0f;
    if (color.w > 0.0f || lighting.w < 1.0f) lighting += float4(color.xyz, color.w > 0.0f ? 1.0f : -1.0f);

    float2 vignetteUv = uv * (1.0f - uv.yx);
    float vignette = pow(15.0f * vignetteUv.x * vignetteUv.y, 0.25f);
    float maxSampleCount = max(lerp(4.0f, 8.0f * min(abs(lighting.w), kGIDenoiser_MaxBlurMask), alphaBlend) * vignette, 1.0f);
    if (lighting.w > maxSampleCount) lighting *= maxSampleCount / lighting.w;

    g_GIDenoiser_BlurMask[did] = blurMask / kGIDenoiser_MaxBlurMask;
    g_GIDenoiser_ColorBuffers[g_Dispatch.x][did] = lighting;
    g_GIDenoiser_ColorDeltaBuffers[g_Dispatch.x][did] = colorDelta;
}

// g_Dispatch: x the colour buffer written, y the one read, z the blur direction (0 across, 1 down),
// w 1 on the last pass: it writes the light, irradiance over pi, to the output.
[numthreads(8, 8, 1)]
void FilterGI(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_BufferDimensions)) return;

    int2 blurDirection = g_Dispatch.z ? int2(0, 1) : int2(1, 0);
    float4 lighting = g_GIDenoiser_ColorBuffers[g_Dispatch.y][did];
    int blurRadius = GIDenoiser_GetBlurRadius(did);
    if (blurRadius > 0)
    {
        float weight = 1.0f;
        float3 color = lighting.xyz / max(lighting.w, 1.0f);
        float centerDepth = toLinearDepth(g_Depth.Load(int3(did, 0)), g_NearFar);
        float3 centerNormal = 2.0f * g_Normal.Load(int3(did, 0)).xyz - 1.0f;
        for (int r = -blurRadius; r <= blurRadius; ++r)
        {
            int2 pos = clamp(int2(did) + r * blurDirection, 0, int2(g_BufferDimensions) - 1);
            float4 c = g_GIDenoiser_ColorBuffers[g_Dispatch.y][pos];
            if (c.w <= 0.0f) continue;

            float depth = toLinearDepth(g_Depth.Load(int3(pos, 0)), g_NearFar);
            float3 normal = 2.0f * g_Normal.Load(int3(pos, 0)).xyz - 1.0f;
            float depthDiff = 1.0f - centerDepth / depth;
            float depthFactor = exp2(-(lighting.w > 0.0f ? 2e2f : 2e1f) * abs(depthDiff));
            float normalFactor = max(dot(normal, centerNormal), 0.0f);
            normalFactor *= normalFactor;
            normalFactor *= normalFactor;
            float w = depthFactor * (lighting.w > 0.0f ? normalFactor : 1.0f);
            color += w * (c.xyz / max(c.w, 1.0f));
            weight += w;
        }

        lighting.xyz = color / weight * max(lighting.w, 1.0f);
    }

    if (g_Dispatch.w) g_Output[did] = float4(lighting.xyz / max(lighting.w, 1.0f) * (INV_PI * g_Frame.nearFar.w), 1.0f);
    g_GIDenoiser_ColorBuffers[g_Dispatch.x][did] = lighting;
}
