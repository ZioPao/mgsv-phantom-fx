#pragma once

#include "passes/ray_tracing/gi_d3d12.hpp"
#include "passes/ray_tracing/omm_layout.hpp"
#include "passes/ray_tracing/scene_memory.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"
#include "range_allocator.hpp"

#include <d3d12.h>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The ray-traced scene on the Direct3D 12 side (docs/DEVELOPMENT.md, "Ray-traced scene"), laid out as
// DXR scenes are: every mesh's triangles in one geometry pool (indexed where the game's are), a
// bottom-level structure per model with a geometry per mesh (opaque geometry flagged so: only cut-out
// geometry runs the alpha test), compacted once built; a table of the geometries the rays reach through
// their instance's first geometry; the game's textures' slots; and the frame's top level of instances.
// The scene contents reach it as their scene memory (scene_memory.hpp). Everything here runs on the
// game's render thread.
namespace phantom_fx::gi_scene
{
using gi_d3d12::ComPtr;
using gi::GIGeometry;
using gi::GIInstanceDesc;
using scene_contents::BlasId;
using scene_contents::Geometry;
using scene_contents::GeometryUploadBytes;
using scene_contents::SceneTexture;
using scene_contents::Use;

constexpr std::uint32_t FramesInFlight = 3;
constexpr std::uint32_t MaxInstances = 1u << 17;
constexpr std::uint32_t MaxGeometries = 1u << 17;

class Scene : public scene_contents::SceneMemory
{
public:
    // The game's textures' slots are GI_MAX_TEXTURES descriptors from `textures` in the frame's
    // shader-visible heap, `descriptorSize` apart.
    Scene(ID3D12Device5* device, D3D12_CPU_DESCRIPTOR_HANDLE textures, UINT descriptorSize);

    // ---- The frame, in this order ----
    // Once the GPU is past the slot's last frame: gives back what the GPU is done with (`completedFence`),
    // texture slots too, and reads the sizes the slot's last builds compact to; `signalValue` ends this
    // frame.
    void beginFrame(std::uint32_t slot, std::uint64_t completedFence, std::uint64_t signalValue);
    // Then the frame's changes (below). Then the static structures this frame builds within its budget,
    // and their compactions: those built are traced this frame.
    void planBuilds() override;
    // Then the frame's instances (addInstance), then the recording: the pool's and table's writes,
    // after which kernels may write the pool (it is in the unordered access state); the bottom levels;
    // the instances' copy, after which kernels may change them (unordered access too); the top level.
    // Between frames the pool, the table and the instances are in the non-pixel shader resource state.
    void recordWrites(ID3D12GraphicsCommandList4& list);
    void recordBottomLevels(ID3D12GraphicsCommandList4& list);
    void recordInstances(ID3D12GraphicsCommandList4& list);
    void recordTopLevel(ID3D12GraphicsCommandList4& list);

    // ---- The geometry pool ----
    // `bytes` of it, 16-byte aligned; nullopt when it has no room.
    std::optional<std::uint64_t> allocate(std::uint64_t bytes) override;
    // Gives a range back once the GPU is past this frame.
    void release(std::uint64_t offset, std::uint64_t bytes) override;
    // Writes `data` at `offset` this frame; false when the frame's upload has no room left.
    bool upload(std::uint64_t offset, std::span<const std::uint8_t> data) override;
    std::uint64_t uploadRoom() const override;

    // ---- Bottom-level structures ----
    // With their memory for the largest build and their rows of the geometry table, in order; nullopt
    // when either has no room. A static one is queued for building.
    std::optional<BlasId> createBlas(Use use, const std::vector<Geometry>& geometries) override;
    // Gone from this frame's instances on; its memory and rows return once the GPU is past the frame.
    void destroyBlas(BlasId id) override;
    // A refitted structure: built again this frame (its vertices moved far), or refitted (a little);
    // false when the frame's scratch has no room: not built again, it is not traced until it is.
    bool rebuild(BlasId id) override;
    bool refit(BlasId id) override;
    // A geometry's material: its flags but GI_GEOMETRY_CUT_OUT (its structure has it), textures and threshold.
    void setMaterial(BlasId id, std::uint32_t geometry, const scene_description::Material& material) override;

    // Traced this frame: built, or built in this frame's list (planBuilds).
    bool ready(BlasId id) const override;
    // Rows of the geometry table made since the last call, whose learnt albedo starts over.
    std::vector<std::uint32_t> takeNewGeometries();

    // ---- The frame's instances ----
    // Under `transform` (object to world rows; null for identity), its InstanceID `kind` (GI_INSTANCE_*):
    // its index in the frame's instances, or nullopt past MaxInstances or when not ready.
    std::optional<std::uint32_t> addInstance(BlasId id, const float* transform, std::uint32_t kind) override;
    std::uint32_t instanceCount() const { return static_cast<std::uint32_t>(instances.size()); }

    // ---- The game's textures ----
    // A slot of the scene's texture range (g_SceneTextures) viewing the copy; nullopt when none is free.
    std::optional<std::uint32_t> openTexture(const SceneTexture& texture) override;
    // The slot is free again once the GPU is past this frame.
    void closeTexture(std::uint32_t textureSlot) override;

    // ---- Opacity micromaps (docs/opacity-micromaps.md) ----
    // From now on, static structures with textured cut-out meshes (omm_layout::takesMicromaps) link
    // micromaps baked from their textures' alpha. Only once every kernel tracing the scene declares its
    // ray queries with RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS: without, their traversal is undefined.
    void enableMicromaps();
    // The frame's bakes, for BakeMicromaps then ResolveMicromapIndices (omm_bake.hlsl) between recordWrites
    // and recordBottomLevels, on the micromap pool at u40. Byte offsets into the pool.
    struct MicromapBake
    {
        std::uint32_t records{}, blocks{}, states{}, data{}; // BakeMicromaps' g_Dispatch
        std::uint32_t triangleBlocks{}, triangles{}, indices{}; // ResolveMicromapIndices' x, y, w (z: states)
    };
    const std::vector<MicromapBake>& micromapBakes() const { return bakes; }
    ID3D12Resource* micromapPool() const { return micromaps.Get(); }
    static constexpr std::uint64_t MicromapPoolBytes = 64ull << 20;

    // ---- The opacity split, where there are no micromaps ----
    // From now on, static structures' textured cut-out meshes are classified by the micromaps' bake,
    // triangle by triangle, and built again as up to two geometries: the wholly opaque triangles flagged
    // opaque (no alpha test), the rest as before; wholly transparent ones are dropped. Not with enableMicromaps.
    void enableOpacitySplit();

    // ---- For the kernels ----
    ID3D12Resource* pool() const { return geometryPool.Get(); }
    ID3D12Resource* geometryTable() const { return table.Get(); }
    ID3D12Resource* instanceBuffer() const { return instanceDescs.Get(); }
    ID3D12Resource* topLevel() const { return tlas.Get(); }
    static constexpr std::uint64_t PoolBytes = 384ull << 20;

    // ---- Diagnostics ----
    struct Stats
    {
        std::uint32_t structures{}, compacted{}, waiting{};
        std::uint32_t builtThisFrame{}, compactedThisFrame{};
        std::uint64_t builtTriangles{}; // This frame
        std::uint64_t compactedFrom{}, compactedTo{}; // Bytes, every compaction so far
        std::uint64_t poolUsed{}, poolCapacity{}, blasUsed{}, blasCapacity{};
        std::uint32_t geometriesUsed{};
        std::uint32_t micromapStructures{}, micromapBlocks{}; // Structures with micromaps, and their blocks
        std::uint64_t micromapTriangles{};                   // Triangles the blocks cover
        std::uint64_t micromapPoolUsed{};
        std::uint64_t micromapBlockBytes{};                  // The blocks' own bytes, padding included
        std::array<std::uint32_t, omm_layout::MaxLevel + 1> micromapLevels{};
        std::uint32_t micromapLinks{}, micromapRelinks{};     // This session
        std::uint32_t splitStructures{};                      // Built again with their meshes split, this session
        std::uint64_t splitOpaque{}, splitTransparent{}, splitMixed{}; // Their triangles, this session
    };
    Stats stats() const;

private:
    // A cut-out mesh's texture coordinates and indices, kept for baking its micromaps when its texture comes.
    struct MicromapSource
    {
        std::vector<std::uint32_t> uvs;
        std::vector<std::uint16_t> indices;
    };
    // A structure's linked micromaps: its room in the micromap pool and its array's in the structure pool,
    // and its geometries as built with them (pointing into `triangles` and `linkages`).
    struct LinkedMicromaps
    {
        std::uint64_t poolOffset{}, poolBytes{};
        std::uint64_t arrayOffset{}, arraySize{};
        std::uint32_t blocks{};
        std::uint32_t triangleCount{};
        std::array<std::uint32_t, omm_layout::MaxLevel + 1> levels{}; // Blocks at each level
        std::vector<D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC> triangles;
        std::vector<D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC> linkages;
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    };
    struct Blas
    {
        bool live{};
        Use use{};
        std::uint32_t generation{};
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
        std::uint32_t firstGeometry{}, triangles{};
        std::uint64_t offset{}, size{}; // In the structure pool
        std::uint64_t scratch{}, updateScratch{}; // Bytes a build and a refit need
        bool built{}, compacted{}, queued{};
        std::uint32_t serial{}; // Changes with its room and geometry: a compacted size read back is for one serial
        std::vector<MicromapSource> sources; // By geometry; empty without any
        bool micromapsDirty{};               // A source's texture changed since its micromaps were baked
        bool pendingLink{};                  // Its array was built last frame: the structure links it now
        std::optional<LinkedMicromaps> micromaps;
        // The opacity split: the game's meshes (the first geometries; split ones' opaque parts follow), the
        // rows held for them, each mesh's opaque part's row (NoRow without), the index lists made for the
        // parts in the geometry pool, each mesh's split done, and a classification on its way back.
        std::uint32_t meshes{}, rowCount{};
        std::vector<std::uint32_t> splitRows;
        std::vector<std::array<std::uint64_t, 2>> splitIndices;
        std::vector<bool> meshSplit;
        bool classifying{};
    };
    static constexpr std::uint32_t NoRow = ~0u;
    // A structure's meshes classified this frame: their blocks' states come back in the slot's readback.
    struct Classification
    {
        BlasId id{};
        std::uint32_t generation{};
        std::uint64_t readbackOffset{};
        std::uint32_t blocks{};
        std::vector<std::uint32_t> meshes, firstTriangle, triangleBlocks;
    };
    // A classification read back: the structure is built again with its meshes split.
    struct SplitDue
    {
        BlasId id{};
        std::uint32_t generation{};
        std::vector<std::uint32_t> meshes;
        std::vector<omm_layout::OpacitySplit> parts;
    };
    // An array the frame builds, before the bottom levels: its inputs in the micromap pool, its histogram.
    struct ArrayBuild
    {
        BlasId id{};
        std::uint32_t generation{};
        std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> histogram;
        std::uint64_t data{}, descs{}; // Pool offsets
        std::uint64_t scratch{};       // Into the frame's scratch
    };

    // What the GPU may still use: given back once it is past the frame that signalled `fenceValue`.
    enum class Pool : std::uint8_t
    {
        Geometry,
        Structures,
        Rows,
        Micromaps
    };
    struct Release
    {
        std::uint64_t fenceValue{};
        Pool pool{};
        std::uint64_t offset{}, size{};
    };

    // The frame's recorded work.
    static constexpr std::uint32_t NoPostbuild = ~0u;
    struct Build
    {
        BlasId id{};
        std::uint32_t generation{};
        bool update{};
        std::uint64_t scratch{}; // Offset into the frame's scratch
        std::uint32_t postbuild{}; // Its compacted size's place in the slot's readback, or NoPostbuild
    };
    struct Compaction
    {
        BlasId id{};
        std::uint64_t from{}, to{};
    };
    // A compacted size read back: the structure compacts in the next frame planned.
    struct Due
    {
        BlasId id{};
        std::uint32_t generation{}, serial{};
        std::uint64_t size{};
    };
    struct Upload
    {
        std::uint64_t poolOffset{}, uploadOffset{}, bytes{};
    };
    // A static structure built from a slot's list: its compacted size comes back with the slot.
    struct Built
    {
        BlasId id{};
        std::uint32_t generation{}, serial{};
    };
    // A texture slot, and the texture it views, the GPU may still use: free again once it is past the
    // frame that signalled `fenceValue`.
    struct ClosedTexture
    {
        std::uint64_t fenceValue{};
        std::uint32_t slot{};
        ComPtr<ID3D12Resource> resource;
    };

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputsOf(const Blas& blas, bool update) const;
    // Bakes the structure's micromaps from its textures as they are now (or unlinks them when none takes
    // any) and moves it to the room its new inputs need, its build recorded this frame without a
    // compacted size; false when this frame has no room for it.
    bool prepareMicromaps(BlasId id);
    bool finishMicromaps(BlasId id);
    // Records the bake of the structure's meshes ready to classify; false when the frame has no room.
    bool classify(BlasId id);
    // Builds the structure again with its meshes split, this frame, `postbuild` its compacted size's place.
    enum class SplitResult : std::uint8_t
    {
        Built,
        Later,
        Dropped
    };
    SplitResult applySplit(SplitDue& due, std::uint32_t postbuild);
    void releaseMicromaps(const LinkedMicromaps& linked);
    D3D12_GPU_VIRTUAL_ADDRESS micromapAddress(std::uint64_t offset) const;
    D3D12_GPU_VIRTUAL_ADDRESS address(const Blas& blas) const;
    void writeRow(std::uint32_t row, const GIGeometry& geometry);
    std::uint64_t reserveUpload(std::uint64_t bytes);
    void writeTexture(std::uint32_t textureSlot, ID3D12Resource* texture, DXGI_FORMAT format, std::uint32_t mips);

    ID3D12Device5* device;
    ComPtr<ID3D12Resource> geometryPool, blasPool, scratch, table, instanceDescs, tlas, postbuildSizes;
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> uploads, postbuildReadback;
    std::array<std::uint8_t*, FramesInFlight> uploadData{};
    std::uint64_t tlasSize{}, tlasScratch{}, uploadUsed{}, geometryUploaded{}, scratchUsed{};
    std::uint32_t slot{};
    std::uint64_t signalValue{};

    RangeAllocator poolAllocator{PoolBytes}, blasAllocator, rowAllocator{MaxGeometries};
    std::vector<Release> releases;
    std::vector<Blas> structures;
    std::vector<BlasId> freeIds, buildQueue;
    std::vector<Due> compactionsDue;
    std::array<std::vector<Built>, FramesInFlight> builtInSlot;

    // The table on the CPU, and its rows written since the last frame; rows made since takeNewGeometries.
    std::vector<GIGeometry> rows = std::vector<GIGeometry>(MaxGeometries);
    std::vector<std::uint32_t> dirtyRows, newRows;

    std::vector<Upload> poolUploads;
    std::vector<Build> builds;
    std::vector<Compaction> compactions;
    std::vector<GIInstanceDesc> instances;
    Stats tally;

    // Opacity micromaps: off until enableMicromaps; the pool, the frame's writes into it, bakes and arrays.
    bool micromapsOn{};
    bool micromapLogged{}; // The first link's line is in the log
    bool micromapLinked{};  // A structure has linked micromaps this session (PHANTOM_FX_OMM_ONCE)
    std::uint32_t micromapLinks{}, micromapRelinks{}; // Links made this session, relinks included
    ComPtr<ID3D12Resource> micromaps;
    // The array builds' own scratch, never the scene's: their place among the scene's builds is what
    // the in-game hang pointed at (docs/opacity-micromaps.md).
    ComPtr<ID3D12Resource> micromapScratch;
    std::uint64_t micromapScratchUsed{};
    static constexpr std::uint64_t MicromapScratchBytes = 8ull << 20;
    RangeAllocator micromapAllocator{MicromapPoolBytes};
    std::vector<Upload> micromapUploads;
    std::vector<MicromapBake> bakes;
    std::vector<ArrayBuild> arrayBuilds;
    // The size of the copy each texture slot views, for the micromaps' levels.
    std::vector<std::array<std::uint32_t, 2>> textureSizes = std::vector<std::array<std::uint32_t, 2>>(GI_MAX_TEXTURES);

    // The opacity split: on instead of micromaps; the classifications in flight by slot, read back into
    // the slot's buffer; the frame's copies into it; those read back, waiting to be built.
    bool splitsOn{};
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> splitReadback;
    std::array<std::vector<Classification>, FramesInFlight> classifying;
    std::vector<Upload> splitCopies; // poolOffset: the micromap pool's; uploadOffset: the readback's
    std::uint64_t splitReadbackUsed{};
    std::vector<SplitDue> splitsDue;
    static constexpr std::uint64_t SplitReadbackBytes = 1ull << 20;
    std::uint32_t splitStructures{};
    std::uint64_t splitOpaque{}, splitTransparent{}, splitMixed{};

    // The game's textures by slot, a descriptor each from `textureDescriptors`; the free slots, and those
    // the GPU may still read.
    D3D12_CPU_DESCRIPTOR_HANDLE textureDescriptors{};
    UINT descriptorSize{};
    std::vector<OpenedTexture> textures = std::vector<OpenedTexture>(GI_MAX_TEXTURES);
    std::vector<std::uint32_t> freeTextures = [] {
        std::vector<std::uint32_t> free;
        for (auto textureSlot = GI_MAX_TEXTURES; textureSlot > 0; --textureSlot) free.push_back(textureSlot - 1);
        return free;
    }();
    std::vector<ClosedTexture> closedTextures;
};
}
