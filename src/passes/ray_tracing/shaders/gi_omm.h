// Opacity micromaps' micro-triangles (docs/opacity-micromaps.md), shared by the layout on the CPU
// (omm_layout.cpp) and the bake kernel, so both place a micro-triangle's state at the same bits.
//
// A base triangle at subdivision level L is cut into 4^L micro-triangles on an N x N barycentric grid
// (N = 2^L). Their states are stored along the space-filling curve the DXR and Vulkan specs define:
// into the sub-triangle nearest vertex 0, the middle one flipped, nearest vertex 1, then nearest vertex
// 2 flipped, recursively. `OmmIndex` follows the Vulkan spec's reference code (appendix
// VK_EXT_opacity_micromap, "Reference Code"). Barycentrics are DXR's: u weighs vertex 1, v vertex 2.
#ifndef GI_OMM_H
#define GI_OMM_H

#ifdef __cplusplus
#include <cstdint>

namespace phantom_fx::gi
{
using uint = std::uint32_t;
#define GI_OMM_INLINE inline
#define GI_OMM_OUT(type) type&
#else
#define GI_OMM_INLINE
#define GI_OMM_OUT(type) out type
#endif

// The finest level the bake makes; the specs allow 12.
#define GI_OMM_MAX_LEVEL 6u

// A micro-triangle's state in a 4-state micromap: two bits, packed from the least significant bit of
// each byte up (D3D12_RAYTRACING_OPACITY_MICROMAP_STATE).
#define GI_OMM_TRANSPARENT         0u
#define GI_OMM_OPAQUE              1u
#define GI_OMM_UNKNOWN_TRANSPARENT 2u
#define GI_OMM_UNKNOWN_OPAQUE      3u
#define GI_OMM_STATE_BITS          2u

// A micro-triangle is known (opaque or transparent) only when every sample of it is this far from the
// threshold; within it, the shader's own test decides (an unknown state).
#define GI_OMM_KNOWN_MARGIN 0.0625f

// What BakeMicromaps finds of a whole block (g_OmmBlockStates): a bit when any of its micro-triangles is
// not transparent, a bit when any is not opaque. Exactly one bit set is a uniform block, whose
// triangles take a special index (ResolveMicromapIndices).
#define GI_OMM_SOME_NOT_TRANSPARENT 1u
#define GI_OMM_SOME_NOT_OPAQUE      2u

// The linkage's special indices (D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX), as unsigned.
#define GI_OMM_INDEX_FULLY_TRANSPARENT 0xFFFFFFFFu
#define GI_OMM_INDEX_FULLY_OPAQUE      0xFFFFFFFEu

// One block to bake (omm_layout::Block with its mesh's texture): the three texture coordinates it
// covers (two halves each, u low), its level, its texture (1 + its slot in g_SceneTextures), where its
// states start in the array's input, and the alpha below which light passes.
struct GIOmmBlock
{
    uint uv0, uv1, uv2;
    uint level;
    uint texture;
    uint byteOffset;
    float threshold;
    uint pad;
};

GI_OMM_INLINE uint OmmSpreadBits(uint x)
{
    x = (x | (x << 8u)) & 0x00ff00ffu;
    x = (x | (x << 4u)) & 0x0f0f0f0fu;
    x = (x | (x << 2u)) & 0x33333333u;
    x = (x | (x << 1u)) & 0x55555555u;
    return x;
}

// The micro-triangle at barycentrics (u, v) of a base triangle at `level`: its place along the curve.
GI_OMM_INLINE uint OmmIndex(float u, float v, uint level)
{
    const uint steps = 1u << level;
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);

    const float fu = u * float(steps);
    const float fv = v * float(steps);
    uint iu = uint(fu);
    uint iv = uint(fv);
    const float uf = fu - float(iu);
    const float vf = fv - float(iv);
    if (iu >= steps) iu = steps - 1u;
    if (iv >= steps) iv = steps - 1u;

    const uint iuv = iu + iv;
    if (iuv >= steps) iu -= iuv - steps + 1u;

    // The third coordinate, inverted, one less on a flipped micro-triangle.
    uint iw = ~(iu + iv);
    if (uf + vf >= 1.0f && iuv < steps - 1u) --iw;

    const uint b0 = ~(iu ^ iw) & (steps - 1u);
    const uint t = (iu ^ iv) & b0;
    uint f = t;
    f ^= f >> 1u;
    f ^= f >> 2u;
    f ^= f >> 4u;
    f ^= f >> 8u;
    const uint b1 = ((f ^ iu) & ~b0) | t;

    return OmmSpreadBits(b0) | (OmmSpreadBits(b1) << 1u);
}

// The bake's threads walk the N x N square of a level's cells; each cell is one micro-triangle, upright
// at grid corner (cell's x, y) when x + y < N, else flipped, at the corner mirrored through the square's
// centre. Gives the micro-triangle's centroid in barycentrics (where the bake samples) and its index.
GI_OMM_INLINE uint OmmCellMicroTriangle(uint cell, uint level, GI_OMM_OUT(float) u, GI_OMM_OUT(float) v)
{
    const uint steps = 1u << level;
    const uint x = cell & (steps - 1u);
    const uint y = cell >> level;
    const float scale = 1.0f / float(steps);
    if (x + y < steps)
    {
        u = (float(x) + 1.0f / 3.0f) * scale;
        v = (float(y) + 1.0f / 3.0f) * scale;
    }
    else
    {
        u = (float(steps - 1u - x) + 2.0f / 3.0f) * scale;
        v = (float(steps - 1u - y) + 2.0f / 3.0f) * scale;
    }

    return OmmIndex(u, v, level);
}

#ifdef __cplusplus
}
#endif

#endif
