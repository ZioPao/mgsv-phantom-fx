// The global illumination's constants and records, shared by gi_renderer.cpp and the HLSL kernels
// in this folder. Every member is 16 bytes wide or packed into 16-byte groups, so HLSL's constant
// buffer packing and C++ agree.
#ifndef GI_SHARED_H
#define GI_SHARED_H

#ifdef __cplusplus
#include <cstdint>

namespace phantom_fx::gi
{
using uint = std::uint32_t;
struct uint2 { uint x, y; };
struct uint4 { uint x, y, z, w; };
struct float4 { float x, y, z, w; };
struct float4x4 { float m[16]; }; // Rows, applied as dot(row, v), as the engine's matrices are
#define GI_ROW_MAJOR
#else
#define GI_ROW_MAJOR row_major
#endif

// Instance flags.
#define GI_INSTANCE_DRAWN        1u // The game drew it this frame, so it is where the scene shows it
#define GI_INSTANCE_DYNAMIC      2u // Captured again every frame (skinned characters)
#define GI_INSTANCE_ALPHA_TESTED 4u // Its texture cuts holes in it (leaves, grass, nets), which the scene's triangles lack

// Instance masks: the global illumination's rays take both; the sun's shadow rays leave the
// alpha-tested meshes to the game's shadow map where it reaches (gi_shadow.hlsl). 0 leaves an
// instance out of every ray.
#define GI_MASK_OPAQUE       1u
#define GI_MASK_ALPHA_TESTED 2u

// Scatter record flags.
#define GI_SCATTER_NEW 1u // The entry is new: what was learnt about it (its albedo) starts over

// Debug views, in GIFrameConstants.counts.z.
#define GI_VIEW_OFF   0u
#define GI_VIEW_SCENE 1u // The traced scene from the camera: its normals, red where it misses the depth

// Counters in the counter buffer.
#define GI_COUNTER_EMPTY_TILES    0u
#define GI_COUNTER_OVERRIDE_TILES 1u
#define GI_COUNTER_LRU            2u
#define GI_COUNTER_MRU            3u
#define GI_COUNTER_LIST_ELEMENTS  4u
#define GI_COUNTER_PACKED_TILES   5u // Two, by GIFrameConstants.hashMisc.y
#define GI_COUNTER_UPDATE_TILES   7u
#define GI_COUNTER_VISIBILITY0    8u // First-bounce hits, from the screen probes' rays
#define GI_COUNTER_VISIBILITY1    9u // Second-bounce hits, from the first-bounce cells' rays
#define GI_COUNTER_LIGHTS         10u // Hits whose light PopulateCells works out
// Diagnostics, read back with the output's statistics.
#define GI_COUNTER_HITS           11u // Probe rays that hit something
#define GI_COUNTER_NEAR_HITS      12u // Probe rays that hit something within 25 cm
#define GI_COUNTER_COPIES         13u // Hits skipped as copies of the surface a ray left
#define GI_COUNTER_FULL_BUCKETS   14u // Hits the hash grid had no room for
#define GI_COUNTER_COUNT          16u
#define GI_COUNTER_NONE           0xFFFFFFFFu

// Slots of the indirect dispatch argument buffer.
#define GI_DISPATCH_SLOTS 16u

struct GIFrameConstants
{
    GI_ROW_MAJOR float4x4 viewProjection; // World to clip, jittered as the depth was drawn
    GI_ROW_MAJOR float4x4 viewProjectionInverse;
    GI_ROW_MAJOR float4x4 previousViewProjection;
    GI_ROW_MAJOR float4x4 previousViewProjectionInverse;
    float4 eye;          // xyz: the camera in the world
    float4 previousEye;
    float4 sunDirection; // xyz: towards the sun, world space; w: tangent of its angular radius
    float4 sunColor;     // rgb: the sun's diffuse light at normal incidence, in the game's units
    float4 skySH[9];     // rgb: the sky's radiance as L2 spherical harmonics, world space
    uint4 dimensions;    // xy: render size; z: frame index; w: instance count
    float4 nearFar;      // x: near plane; y: far plane; z: albedo where none is known; w: output scale (the exposure)
    uint4 counts;        // x: scatter records; y: entries whose bounds start over; z: debug view; w: 1 on a fresh start (no last frame)
    // GI-1.2's screen probes and hash grid cache (AMD Capsaicin's ScreenProbesConstants and
    // HashGridCacheConstants).
    uint4 probeLayout;        // x: probe size; y: spawn tile size; z, w: probes across and down
    uint4 probeLimits;        // x: probe mask mips; y: probes that can spawn; z: rays; w: frame parity
    float4 cellSizes;         // x: screen probe cell size; y: hash cell size; z: smallest hash cell; w: hash tile size
    uint4 hashCounts;         // x: buckets; y: tiles; z: cells; w: tiles per bucket
    uint4 hashTileSizes;      // Cells across a tile, mips 0 to 3
    uint4 hashTileCells;      // Cells in a tile, mips 0 to 3
    uint4 hashTileFirstCells; // First cell of each mip in a tile
    uint4 hashMisc;           // x: cells in a tile, all mips; y: packed tile parity; z: samples a cell takes a frame
    float4 hashSamples;       // x: most samples; y: chance a multibounce ray is not traced; z: most multibounce samples; w: tile cell ratio
    // The sun's ray-traced shadows (gi_shadow.hlsl).
    float4 shadowRange;  // The game's shadow map range (cPSLight): x: centre depth (c8.x); y: start distance (c8.y); z: fade rate (c6.x); w: 1 once known
    float4 shadowParams; // x: tangent of the sun's angular radius; y: how much sun an alpha-tested mesh blocks; z: ray length; w: 1 on a fresh start
};

// One captured draw on its way from the transfer texture into the vertex pool.
struct GIScatterRecord
{
    uint entry;
    uint transferOffset; // First vertex in the transfer texture, rows of GI_TRANSFER_WIDTH
    uint vertexCount;    // Three per triangle
    uint poolOffset;     // First vertex in the pool
    uint flags;          // GI_SCATTER_*
    uint padding[3];
};

// One instance of the frame's acceleration structure.
struct GIInstanceRecord
{
    uint entry;
    uint vertexOffset; // First vertex in the pool
    uint triangleCount;
    uint flags;        // GI_INSTANCE_*
    uint2 blas;        // GPU address of its bottom-level acceleration structure
    uint2 padding;
};

// D3D12_RAYTRACING_INSTANCE_DESC.
struct GIInstanceDesc
{
    float4 transform[3];
    uint idAndMask;         // InstanceID in the low 24 bits, InstanceMask in the high 8
    uint hitGroupAndFlags;
    uint2 blas;
};

#define GI_TRANSFER_WIDTH 4096u

#ifdef __cplusplus
static_assert(sizeof(GIFrameConstants) % 16 == 0);
static_assert(sizeof(GIInstanceDesc) == 64);
}
#endif

#endif
