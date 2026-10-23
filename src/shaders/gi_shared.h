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

// What an instance is: the low 8 bits of its InstanceID hold these; a captured mesh's (and the frame's
// skinned meshes') InstanceID holds its entry above (GI_INSTANCE_ENTRY_SHIFT).
#define GI_INSTANCE_DRAWN    1u  // Captured: the game drew it this frame or lately (gi_renderer.cpp, DrawnGraceFrames), so it is where the scene shows it
#define GI_INSTANCE_DYNAMIC  2u  // Captured again every frame (skinned characters)
#define GI_INSTANCE_CAPTURED 4u  // A static mesh captured from the game's draws (gi_capture.cpp), world space, kept while the game draws it
#define GI_INSTANCE_MODEL    8u  // One of the game's models (engine_scene.hpp), in object space under its transform
#define GI_INSTANCE_TERRAIN  16u // A ring of the game's terrain, made from its heightmap (TerrainGrid)
#define GI_INSTANCE_KINDS       0xFFu
#define GI_INSTANCE_ENTRY_SHIFT 8u

// What a geometry is (GIGeometry::flags).
#define GI_GEOMETRY_CUT_OUT       1u // Its texture cuts holes in it (leaves, nets, a pylon's lattice): non-opaque in its structure
#define GI_GEOMETRY_UV            2u // Its vertices' fourth floats hold their texture coordinates, two halves
#define GI_GEOMETRY_ALPHA_TEXTURE 4u // A cut-out geometry with its texture: the rays test its alpha (GIGeometry::alphaThreshold)
#define GI_GEOMETRY_MASK_IN_RED   8u // Its texture is a mask (red), not a colour with alpha

// The game's textures the scene holds at most (g_SceneTextures, gi_textures.hpp).
#define GI_MAX_TEXTURES 4096u

// GIGeometry::indexOffset of a geometry without indices: three vertices a triangle.
#define GI_NO_INDICES 0xFFFFFFFFu

// A hit the global illumination keeps (gi_light.hlsl) packs its instance into the low bits of a uint
// and its geometry within the instance's structure above: 2^20 instances, 4096 geometries a structure.
#define GI_HIT_INSTANCE_BITS 20u

// The terrain's rings (TerrainGrid): so many, each so many cells across.
#define GI_TERRAIN_RINGS 4u
#define GI_TERRAIN_CELLS 256u

// Instance masks: the global illumination's rays take GI_MASK_TRACED. A captured cut-out mesh has no
// texture coordinates, so no alpha: inside the game's shadow map range the sun's shadow rays leave it to
// the shadow map, beyond it they take it (gi_shadow.hlsl). 0 leaves an instance out of every ray.
#define GI_MASK_SCENE            1u
#define GI_MASK_CAPTURED_CUT_OUT 2u
#define GI_MASK_TRACED           3u
#define GI_MASK_HIDDEN           0x80u // Left out of the scene (CullCaptured); only the shadow debug view's rays see it

// Why an instance is in the scene or left out (CullCaptured), for the shadow debug view and probe.
#define GI_REASON_DRAWN       0u // The game drew it this frame or lately
#define GI_REASON_OUT_OF_VIEW 1u // Not drawn, not all of it in view: kept
#define GI_REASON_BEHIND      2u // Not drawn, all of it in view but behind what the game drew (culled): kept
#define GI_REASON_LEFT_OUT    3u // Not drawn, all of it in view, something of it would show: left out
#define GI_REASON_NONE        0xFFFFFFFFu

// A captured mesh's state for the shadow probe, by entry (from the CPU, each frame the probe runs): x
// the bits below, the frames since the game last drew it in bits 8 to 19 and the frames it was drawn
// in bits 20 to 31; y its instance this frame, or GI_REASON_NONE.
#define GI_ENTRY_LIVE         1u
#define GI_ENTRY_BUILT        2u
#define GI_ENTRY_ALPHA_TESTED 4u
#define GI_ENTRY_INSTANCED    8u

// Captured meshes whose bounds the probe's sun ray crosses, listed at most.
#define GI_PROBE_CROSSINGS 256u
// The shadow probe's global illumination rays, one a thread of its group.
#define GI_PROBE_LIGHT_RAYS 64u

// Scatter record flags.
#define GI_SCATTER_NEW 1u // The geometry is new: what was learnt about it (its albedo) starts over

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
// The frame's largest value (asuint of a float, its largest channel) at each step of the light
// (MaxDiagnostic in gi_light.hlsl), to find where light grows past what the sun and sky give.
#define GI_COUNTER_MAX_ALBEDO       16u // A hit's albedo
#define GI_COUNTER_MAX_DIRECT       17u // A hit's direct light (PopulateCells)
#define GI_COUNTER_MAX_CELL         18u // A cell's direct light, after its update (UpdateTiles)
#define GI_COUNTER_MAX_INDIRECT     19u // A cell's indirect light, after its update
#define GI_COUNTER_MAX_BOUNCE       20u // A second bounce's light into its first-bounce cell (UpdateMultibounceCells)
#define GI_COUNTER_MAX_RESOLVED     21u // A probe ray's light from the cache (ResolveCells)
#define GI_COUNTER_MAX_PROBE        22u // A probe cell's light, blended with its history (BlendScreenProbes)
#define GI_COUNTER_MAX_INTERPOLATED 23u // A pixel's light from the probes (InterpolateScreenProbes)
#define GI_COUNTER_MAX_OUTPUT       24u // A pixel's denoised light, before the exposure (FilterGI)
#define GI_COUNTER_MAXIMA_FIRST     16u
#define GI_COUNTER_MAXIMA           9u
#define GI_COUNTER_COUNT            32u // The last: the frame's probes
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
    // The game's terrain (TerrainGrid in gi_scene.hlsl): a world point is (u, height, v) x scale +
    // offset, u and v its virtual heightmap coordinates; the page table's converter (the terrain
    // shaders' b4 c0 and c1).
    float4 terrainScale;        // xyz: the terrain shaders' b5 c10
    float4 terrainOffset;       // xyz: b5 c11
    float4 terrainUvConverter;  // b4 c0
    float4 terrainTexelCorrect; // xy: b4 c1.xy
    float4 terrainRings[GI_TERRAIN_RINGS]; // xy: the ring's lowest corner (world x, z); z: cell size; w: cells across
    uint4 terrainRingOffsets;   // Each ring's first vertex, in bytes into the geometry pool
    // Ray cones for the textures' levels of detail (gi_common.hlsli, TextureLod): x the angle a pixel
    // spans, radians.
    float4 rayCone;
};

// One captured draw on its way from the transfer texture into the geometry pool, its entry's bounds
// starting over; with no vertices, only a geometry whose albedo starts over (GI_SCATTER_NEW).
struct GIScatterRecord
{
    uint entry;
    uint transferOffset; // First vertex in the transfer texture, rows of GI_TRANSFER_WIDTH
    uint vertexCount;    // Three per triangle
    uint poolOffset;     // First vertex, in bytes into the geometry pool
    uint geometry;       // With GI_SCATTER_NEW: the geometry's row of the table
    uint flags;          // GI_SCATTER_*
    uint padding[2];
};

// One geometry of a bottom-level structure (gi_scene.hpp): where its triangles lie in the geometry
// pool, and its material. An instance's geometries start at its InstanceContributionToHitGroupIndex,
// in its structure's order.
struct GIGeometry
{
    uint vertexOffset;    // Bytes into the pool: 16 a vertex, xyz its position, w its texture coordinates with GI_GEOMETRY_UV
    uint indexOffset;     // Bytes into the pool: 16-bit indices, three a triangle; or GI_NO_INDICES
    uint flags;           // GI_GEOMETRY_*
    uint texture;         // 1 + its base texture's index in g_SceneTextures (its colour, and a cut-out geometry's alpha), 0 without
    float alphaThreshold; // With GI_GEOMETRY_ALPHA_TEXTURE: the alpha below which light passes
    uint padding[3];
};

// The shadow probe: the sun ray from the screen's centre pixel, as the shadow rays trace it but
// towards the sun's centre, with everything that decides it (ProbeShadows in gi_shadow.hlsl).
struct GIShadowProbe
{
    float4 receiver; // xyz: the centre pixel's surface, world space; w: its depth (0 is the sky)
    float4 normal;   // xyz: its normal; w: its distance from the eye
    float4 ray;      // xyz: towards the sun's centre; w: how far the pixel lies beyond the game's shadow map (0 to 1)
    float4 segments; // x: the near segment's end; y: the copy tolerance; z: the near hit's distance; w: the far hit's distance
    uint4 hits;      // x: copies of the surface skipped near; y: the near hit's instance + 1; z: the far hit's (shadow rays' masks) + 1; w: the far hit's (every instance, the left-out too) + 1
    uint4 hitEntry;  // Of the far hit with every instance: x its entry (captured) or geometry's row, y its reason, z its kind (GI_INSTANCE_*)
                     // | its geometry's flags << 16, w its entry's state (GI_ENTRY_*)
    float4 hitBox;   // Of the same: x its box's nearest depth, y the farthest depth over its screen rectangle, z the pyramid level read, w 1 when all of it is in view
    float4 terrain;  // Of the same on the terrain, its triangle's highest corner: x its height (as the ring was last made), y the heightmap's there
                     // now, z the level of detail resident there now; w the frame's index (GIFrameConstants::dimensions.z)
    // The centre pixel's global illumination rays (GI_PROBE_LIGHT_RAYS, cosine-weighted, traced as the probes' are):
    uint4 lightRays;    // x those that hit within 1 m, y within 10 m, z that get away, w that hit a back face
    uint4 lightNearest; // Their nearest hit, and below the camera's ray through the centre pixel (the scene view's): x its
                        // instance + 1, y its geometry | 1 << 31 on a front face, z its kind (GI_INSTANCE_*), w asuint(its distance)
    uint4 view;
    uint4 reasons;   // Instances by reason (GI_REASON_*)
    float4 shadow;   // x: the denoised shadow at the centre; y: this frame's ray (1 lit, 0 blocked)
    uint4 crossings; // x: captured meshes whose bounds the ray crosses; y: of them listed after the probe, each a uint4: x entry,
                     // y asuint(where the ray enters its bounds), z its state (GI_ENTRY_*), w its instance's reason
};

// D3D12_RAYTRACING_INSTANCE_DESC: the frame's instances as the top level holds them, which the
// kernels read too.
struct GIInstanceDesc
{
    float4 transform[3];   // Object to world, rows applied as dot(row, float4(v, 1)); identity for captured meshes
    uint idAndMask;        // InstanceID (GI_INSTANCE_*, and the entry) in the low 24 bits, InstanceMask (GI_MASK_*) in the high 8
    uint hitGroupAndFlags; // InstanceContributionToHitGroupIndex (its first geometry's slot) in the low 24, flags in the high 8
    uint2 blas;
};

#define GI_TRANSFER_WIDTH 4096u

#ifdef __cplusplus
static_assert(sizeof(GIFrameConstants) % 16 == 0);
static_assert(sizeof(GIInstanceDesc) == 64);
static_assert(sizeof(GIGeometry) == 32);
}
#endif

#endif
