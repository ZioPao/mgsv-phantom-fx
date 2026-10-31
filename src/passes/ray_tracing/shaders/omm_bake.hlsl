// The opacity micromaps' bake (docs/opacity-micromaps.md, "Baking"): each block's micro-triangle states
// from its texture's alpha, written into the micromap array's input, and its triangles' indices into
// the array, uniform blocks turned into special indices. Bound as the global illumination's other
// kernels are (gi_renderer.cpp's root signature), on registers none of them uses.
#include "gi_shared.h"
#include "gi_omm.h"

cbuffer DispatchConstants : register(b1)
{
    uint4 g_Dispatch; // BakeMicromaps: x the first block, y the blocks. ResolveMicromapIndices: x the first triangle, y the triangles, z the mesh's first block
};

StructuredBuffer<GIOmmBlock> g_OmmBlocks : register(t26);
StructuredBuffer<uint> g_OmmTriangleBlocks : register(t27); // Each triangle's block within its mesh (omm_layout::Layout::triangleBlocks)
Texture2D<float4> g_SceneTextures[GI_MAX_TEXTURES] : register(t0, space2);

RWByteAddressBuffer g_OmmData : register(u40);         // The micromap array's input
RWStructuredBuffer<uint> g_OmmBlockStates : register(u41); // GI_OMM_SOME_* of each block
RWStructuredBuffer<uint> g_OmmIndices : register(u42);     // The linkage's index buffer

SamplerState g_WrapSampler : register(s2);

#define BAKE_THREADS 64u
#define BAKE_MAX_WORDS ((1u << (2u * GI_OMM_MAX_LEVEL)) * GI_OMM_STATE_BITS / 32u)

groupshared uint g_Words[BAKE_MAX_WORDS];
groupshared uint g_Found;

float2 UvOf(uint packed)
{
    return f16tof32(uint2(packed, packed >> 16));
}

float2 BlockUv(GIOmmBlock block, float u, float v)
{
    return (1.0f - u - v) * UvOf(block.uv0) + u * UvOf(block.uv1) + v * UvOf(block.uv2);
}

// The level of detail whose texels are about a micro-triangle across: its share of the block's texels.
float MicroTriangleLod(GIOmmBlock block, float2 size)
{
    float2 a = UvOf(block.uv0), b = UvOf(block.uv1), c = UvOf(block.uv2);
    float texels = 0.5f * abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * size.x * size.y;
    float perMicroTriangle = texels / float(1u << (2u * block.level));
    return 0.5f * log2(max(perMicroTriangle, 1.0f));
}

// A micro-triangle's state from its three corners and its centroid: known where all four agree past the
// margin, else unknown, leaning as its centroid does.
uint MicroTriangleState(GIOmmBlock block, Texture2D<float4> texture, float lod, float u, float v, uint level)
{
    // The centroid sits a third of a step in from an upright micro-triangle's corner, two thirds in
    // from a flipped one's (OmmCellMicroTriangle).
    float step = 1.0f / float(1u << level);
    bool upright = frac(u / step) < 0.5f;
    float third = step / 3.0f;
    float2 corners[3];
    if (upright)
    {
        corners[0] = float2(u - third, v - third);
        corners[1] = float2(u + 2.0f * third, v - third);
        corners[2] = float2(u - third, v + 2.0f * third);
    }
    else
    {
        corners[0] = float2(u + third, v - 2.0f * third);
        corners[1] = float2(u - 2.0f * third, v + third);
        corners[2] = float2(u + third, v + third);
    }

    float centre = texture.SampleLevel(g_WrapSampler, BlockUv(block, u, v), lod).a;
    float lowest = centre, highest = centre;
    [unroll] for (uint i = 0; i < 3; ++i)
    {
        float alpha = texture.SampleLevel(g_WrapSampler, BlockUv(block, corners[i].x, corners[i].y), lod).a;
        lowest = min(lowest, alpha);
        highest = max(highest, alpha);
    }

    if (lowest >= block.threshold + GI_OMM_KNOWN_MARGIN) return GI_OMM_OPAQUE;
    if (highest < block.threshold - GI_OMM_KNOWN_MARGIN) return GI_OMM_TRANSPARENT;
    return centre >= block.threshold ? GI_OMM_UNKNOWN_OPAQUE : GI_OMM_UNKNOWN_TRANSPARENT;
}

// A thread group a block: its threads walk the level's cells, gather the states in shared memory and
// write the block's words.
[numthreads(BAKE_THREADS, 1, 1)]
void BakeMicromaps(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    if (group.x >= g_Dispatch.y) return;

    GIOmmBlock block = g_OmmBlocks[g_Dispatch.x + group.x];
    uint level = min(block.level, GI_OMM_MAX_LEVEL);
    uint cells = 1u << (2u * level);
    uint words = max(cells * GI_OMM_STATE_BITS / 32u, 1u);

    for (uint w = thread; w < words; w += BAKE_THREADS) g_Words[w] = 0u;
    if (thread == 0) g_Found = 0u;
    GroupMemoryBarrierWithGroupSync();

    Texture2D<float4> texture = g_SceneTextures[NonUniformResourceIndex(block.texture - 1u)];
    float2 size;
    texture.GetDimensions(size.x, size.y);
    float lod = MicroTriangleLod(block, size);

    uint found = 0u;
    for (uint cell = thread; cell < cells; cell += BAKE_THREADS)
    {
        float u, v;
        uint index = OmmCellMicroTriangle(cell, level, u, v);
        uint state = MicroTriangleState(block, texture, lod, u, v, level);
        uint bit = index * GI_OMM_STATE_BITS;
        InterlockedOr(g_Words[bit >> 5], state << (bit & 31u));
        found |= (state != GI_OMM_TRANSPARENT ? GI_OMM_SOME_NOT_TRANSPARENT : 0u) | (state != GI_OMM_OPAQUE ? GI_OMM_SOME_NOT_OPAQUE : 0u);
    }
    InterlockedOr(g_Found, found);
    GroupMemoryBarrierWithGroupSync();

    for (uint w = thread; w < words; w += BAKE_THREADS) g_OmmData.Store(block.byteOffset + 4u * w, g_Words[w]);
    if (thread == 0) g_OmmBlockStates[g_Dispatch.x + group.x] = g_Found;
}

// A thread a triangle: its block's place within the mesh, or the special index of a uniform block (the
// linkage adds the mesh's base location to the rest).
[numthreads(64, 1, 1)]
void ResolveMicromapIndices(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Dispatch.y) return;

    uint block = g_OmmTriangleBlocks[g_Dispatch.x + id.x];
    uint found = g_OmmBlockStates[g_Dispatch.z + block];
    uint index = block;
    if (found == GI_OMM_SOME_NOT_TRANSPARENT) index = GI_OMM_INDEX_FULLY_OPAQUE;
    else if (found == GI_OMM_SOME_NOT_OPAQUE) index = GI_OMM_INDEX_FULLY_TRANSPARENT;
    g_OmmIndices[g_Dispatch.x + id.x] = index;
}
