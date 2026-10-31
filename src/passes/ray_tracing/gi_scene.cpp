// The ray-traced scene on the Direct3D 12 side; gi_scene.hpp describes it.
#include "passes/ray_tracing/gi_scene.hpp"

#include "passes/ray_tracing/omm_layout.hpp"
#include "passes/ray_tracing/shaders/gi_omm.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace phantom_fx::gi_scene
{
namespace
{
using namespace gi_d3d12;

// The structures' memory, and the scratch the frame's static builds share after the top level's.
constexpr std::uint64_t BlasPoolBytes = 384ull << 20;
constexpr std::uint64_t BuildScratchBytes = 64ull << 20;
// Each frame slot's upload buffer: the pool's writes, the table's, the instances.
constexpr std::uint64_t UploadBytes = GeometryUploadBytes + std::uint64_t{MaxInstances} * sizeof(GIInstanceDesc) + (8ull << 20);
// The static structures a frame builds at most: the rest wait for the next frames, so a new area
// costs a few frames a little rather than one frame a lot.
constexpr std::uint32_t BuildsPerFrame = 128;
constexpr std::uint64_t BuildTrianglesPerFrame = 300'000;
// Opacity micromaps: the most a mesh's blocks take (omm_layout::Mesh::maxBytes), and the structures a
// frame links them to at most.
constexpr std::uint64_t MicromapBytesPerMesh = 4ull << 20;
// One structure a frame, as the batch of them (6 at once, 7 in the tests) faulted the GPU with the
// micromaps disabled at traversal too (docs/opacity-micromaps.md).
constexpr std::uint32_t MicromapLinksPerFrame = 1;
// The opacity split's bakes a frame (their states share the slot's readback, SplitReadbackBytes).
constexpr std::uint32_t SplitClassificationsPerFrame = 8;
// An opacity split part without a list.
constexpr std::uint64_t NoList = ~0ull;
// The blocks the scene may carry at once (PHANTOM_FX_OMM_MAX_BLOCKS overrides): about 110 bytes each in
// the micromap pool with their descriptors, records and states, so the pool, not the device, bounds it.
// A structure is only linked while its triangles more keep the total under this. (The cap of eight
// structures this replaced hid a compaction bug, not a device limit: 2026-10-31.)
constexpr std::uint32_t MicromapBlocks = 100000;
// D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT, which the Windows SDK's 26100 headers lack: an
// array's input starts on it.
constexpr std::uint64_t MicromapArrayAlignment = 128;
}

Scene::Scene(ID3D12Device5* device, D3D12_CPU_DESCRIPTOR_HANDLE textures, UINT descriptorSize)
    : device(device), blasAllocator(BlasPoolBytes), textureDescriptors(textures), descriptorSize(descriptorSize)
{
    geometryPool = createBuffer(device, PoolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the geometry pool");
    blasPool = createBuffer(device, BlasPoolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the acceleration structure pool");
    table = createBuffer(device, std::uint64_t{MaxGeometries} * sizeof(GIGeometry), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_FLAG_NONE, "Creating the geometry table");
    instanceDescs = createBuffer(device, std::uint64_t{MaxInstances} * sizeof(GIInstanceDesc), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the instance list");
    postbuildSizes = createStorage(device, std::uint64_t{BuildsPerFrame} * 8, "Creating the compacted sizes");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = MaxInstances;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    tlasSize = prebuild.ResultDataMaxSizeInBytes;
    tlasScratch = aligned(prebuild.ScratchDataSizeInBytes, 256);
    tlas = createBuffer(device, tlasSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the scene's acceleration structure");
    scratch = createStorage(device, tlasScratch + BuildScratchBytes, "Creating the build scratch");

    for (std::uint32_t i = 0; i < FramesInFlight; ++i)
    {
        uploads[i] = createBuffer(device, UploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            "Creating the scene's upload buffer");
        void* data{};
        check(uploads[i]->Map(0, nullptr, &data), "Mapping the scene's upload buffer");
        uploadData[i] = static_cast<std::uint8_t*>(data);
        postbuildReadback[i] = createBuffer(device, std::uint64_t{BuildsPerFrame} * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_NONE, "Creating the compacted sizes' readback");
    }

    for (std::uint32_t textureSlot = 0; textureSlot < GI_MAX_TEXTURES; ++textureSlot) writeTexture(textureSlot, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
}

void Scene::beginFrame(std::uint32_t frameSlot, std::uint64_t completedFence, std::uint64_t frameSignal)
{
    slot = frameSlot;
    signalValue = frameSignal;
    std::erase_if(releases, [&](const Release& release) {
        if (release.fenceValue > completedFence) return false;

        if (release.pool == Pool::Geometry) poolAllocator.release(release.offset, release.size);
        else if (release.pool == Pool::Structures) blasAllocator.release(release.offset, release.size);
        else if (release.pool == Pool::Rows) rowAllocator.release(release.offset, release.size);
        else if (release.pool == Pool::Micromaps) micromapAllocator.release(release.offset, release.size);
        return true;
    });
    std::erase_if(closedTextures, [&](const ClosedTexture& closed) {
        if (closed.fenceValue > completedFence) return false;

        freeTextures.push_back(closed.slot);
        return true;
    });

    // The slot's last static builds wrote their compacted sizes; the GPU is past them.
    auto& built = builtInSlot[slot];
    if (!built.empty())
    {
        void* data{};
        const D3D12_RANGE range{0, built.size() * 8};
        if (SUCCEEDED(postbuildReadback[slot]->Map(0, &range, &data)))
        {
            const auto* sizes = static_cast<const std::uint64_t*>(data);
            for (std::size_t i = 0; i < built.size(); ++i)
                if (sizes[i]) compactionsDue.push_back({built[i].id, built[i].generation, built[i].serial, sizes[i]});

            const D3D12_RANGE none{};
            postbuildReadback[slot]->Unmap(0, &none);
        }

        built.clear();
    }

    // The slot's last classifications: their blocks' states sort each mesh's triangles, and the structure
    // is built again with them (planBuilds). One gone or remade since keeps nothing of it.
    if (auto& classified = classifying[slot]; !classified.empty())
    {
        void* data{};
        const D3D12_RANGE range{0, SplitReadbackBytes};
        if (SUCCEEDED(splitReadback[slot]->Map(0, &range, &data)))
        {
            for (const auto& classification : classified)
            {
                auto& blas = structures[classification.id];
                if (!blas.live || blas.generation != classification.generation) continue;

                blas.classifying = false;
                const std::span states(reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(data) + classification.readbackOffset),
                    classification.blocks);
                SplitDue due{classification.id, classification.generation, classification.meshes, {}};
                for (std::size_t k = 0; k < classification.meshes.size(); ++k)
                {
                    const auto& indices = blas.sources[classification.meshes[k]].indices;
                    const std::span triangleBlocks(classification.triangleBlocks.data() + classification.firstTriangle[k], indices.size() / 3);
                    due.parts.push_back(omm_layout::splitByOpacity(indices, triangleBlocks, states));
                }
                splitsDue.push_back(std::move(due));
            }

            const D3D12_RANGE none{};
            splitReadback[slot]->Unmap(0, &none);
        }
        else
            for (const auto& classification : classified)
                if (auto& blas = structures[classification.id]; blas.generation == classification.generation)
                {
                    blas.classifying = false;
                    blas.micromapsDirty = true;
                }

        classified.clear();
    }
    splitCopies.clear();
    splitReadbackUsed = 0;

    poolUploads.clear();
    micromapUploads.clear();
    bakes.clear();
    arrayBuilds.clear();
    builds.clear();
    compactions.clear();
    instances.clear();
    uploadUsed = geometryUploaded = 0;
    scratchUsed = tlasScratch;
    micromapScratchUsed = 0;
    tally.builtThisFrame = tally.compactedThisFrame = 0;
    tally.builtTriangles = 0;
}

void Scene::planBuilds()
{
    std::uint32_t staticBuilds{};
    std::uint64_t triangles{};

    // The development switches, read once: these run whenever a structure links. `NOBAKE`'s is read in
    // the renderer; the rest are here.
    static const bool once = GetEnvironmentVariableA("PHANTOM_FX_OMM_ONCE", nullptr, 0) != 0;
    static const bool keepFirstLink = GetEnvironmentVariableA("PHANTOM_FX_OMM_NO_RELINK", nullptr, 0) != 0;
    static const auto blockBudget = [] {
        char text[16]{};
        return GetEnvironmentVariableA("PHANTOM_FX_OMM_MAX_BLOCKS", text, sizeof(text)) ? static_cast<std::uint32_t>(std::atoi(text)) : MicromapBlocks;
    }();

    // Structures whose cut-out meshes' textures changed take micromaps baked from them first: a built one
    // is built again with them this frame, a queued one waits for its first build as before.
    if (micromapsOn)
    {
        // A structure whose array was built last frame links it now, and is the only structure built
        // with micromaps this frame (docs/opacity-micromaps.md, "In game").
        for (BlasId id = 0; id < structures.size() && staticBuilds < BuildsPerFrame; ++id)
        {
            auto& blas = structures[id];
            if (!blas.live || !blas.pendingLink || blas.use != Use::Static) continue;
            if (finishMicromaps(id))
            {
                ++staticBuilds;
                triangles += blas.triangles;
            }
        }

        // The smallest meshes first: the budget goes to the many small cut-outs the rays meet, not to one
        // 600-block monster that would spend it alone.
        std::vector<BlasId> candidates;
        for (BlasId id = 0; id < structures.size(); ++id)
        {
            const auto& blas = structures[id];
            // Built and out of the queue: one still waiting would be built twice in a frame, by its link
            // and by the queue, into the same room.
            if (!blas.live || !blas.micromapsDirty || blas.pendingLink || blas.use != Use::Static || !blas.built || blas.queued) continue;
            candidates.push_back(id);
        }
        std::sort(candidates.begin(), candidates.end(), [&](BlasId a, BlasId b) { return structures[a].triangles < structures[b].triangles; });
        // PHANTOM_FX_OMM_MAX_STRUCTURES caps the structures carrying micromaps (none by default).
        static const auto MaxStructures = [] {
            char text[16]{};
            return GetEnvironmentVariableA("PHANTOM_FX_OMM_MAX_STRUCTURES", text, sizeof(text)) ? static_cast<std::uint32_t>(std::atoi(text)) : ~0u;
        }();
        std::uint32_t carrying{};
        for (const auto& other : structures)
            if (other.live && (other.micromaps || other.pendingLink)) ++carrying;

        std::uint32_t liveBlocks{};
        for (const auto& other : structures)
            if (other.live && other.micromaps) liveBlocks += other.micromaps->blocks;

        std::uint32_t prepared{};
        for (const auto id : candidates)
        {
            if (prepared >= MicromapLinksPerFrame) break;
            auto& blas = structures[id];

            // PHANTOM_FX_OMM_ONCE links one structure a session, for telling one link from a batch of them.
            if (once && micromapLinked) return;
            // PHANTOM_FX_OMM_NO_RELINK leaves a structure's first micromaps in place: tells a fault in
            // re-linking (release and rebuild) from one in the first link.
            if (blas.micromaps && keepFirstLink)
            {
                blas.micromapsDirty = false;
                continue;
            }

            // Its triangles bound its blocks (a block a triangle at most), so the budget cannot overshoot.
            if (!blas.micromaps && (carrying >= MaxStructures || liveBlocks + blas.triangles > blockBudget))
            {
                blas.micromapsDirty = false;
                continue;
            }

            if (!prepareMicromaps(id)) continue;
            blas.micromapsDirty = false;
            ++prepared;
            ++micromapLinks;
            if (blas.micromaps)
            {
                liveBlocks += blas.micromaps->blocks;
                ++carrying;
            }
        }
    }
    else if (splitsOn)
    {
        // Classifications read back: their structures built again, split, within the frame's builds.
        std::vector<SplitDue> later;
        for (auto& due : splitsDue)
        {
            const auto& blas = structures[due.id];
            if (staticBuilds >= BuildsPerFrame || triangles >= BuildTrianglesPerFrame)
            {
                later.push_back(std::move(due));
                continue;
            }

            const auto postbuild = static_cast<std::uint32_t>(builtInSlot[slot].size());
            const auto result = applySplit(due, postbuild);
            if (result == SplitResult::Later) later.push_back(std::move(due));
            if (result != SplitResult::Built) continue;

            builtInSlot[slot].push_back({due.id, blas.generation, blas.serial});
            ++staticBuilds;
            triangles += blas.triangles;
        }
        splitsDue = std::move(later);

        // Then the structures whose meshes' textures came: a bake each, read back with the slot.
        std::uint32_t classified{};
        for (BlasId id = 0; id < structures.size() && classified < SplitClassificationsPerFrame; ++id)
        {
            auto& blas = structures[id];
            if (!blas.live || !blas.micromapsDirty || blas.classifying || blas.use != Use::Static || !blas.built || blas.queued) continue;
            if (!classify(id)) break;
            ++classified;
        }
    }

    std::vector<BlasId> waiting;
    for (const auto id : buildQueue)
    {
        auto& blas = structures[id];
        if (!blas.live || !blas.queued) continue;

        if (staticBuilds >= BuildsPerFrame || triangles >= BuildTrianglesPerFrame || scratchUsed + blas.scratch > tlasScratch + BuildScratchBytes)
        {
            waiting.push_back(id);
            continue;
        }

        // The readback's k-th compacted size is the k-th build that asks for one: a build with micromaps
        // asks for none, so it takes no index (an index without its entry handed every later structure
        // its neighbour's size, and the compaction overran its room: 2026-10-31, the device hangs).
        const auto postbuild = static_cast<std::uint32_t>(builtInSlot[slot].size());
        builds.push_back({id, blas.generation, false, scratchUsed, postbuild});
        if (!blas.micromaps) builtInSlot[slot].push_back({id, blas.generation, blas.serial});
        ++staticBuilds;
        scratchUsed += blas.scratch;
        triangles += blas.triangles;
        blas.queued = false;
        blas.built = true;
    }

    buildQueue = std::move(waiting);
    tally.waiting = static_cast<std::uint32_t>(buildQueue.size());

    // Built structures copied into the room their compacted size needs; the room they leave returns
    // once the GPU is past this frame, whose instances use the copies.
    for (const auto& due : compactionsDue)
    {
        auto& blas = structures[due.id];
        // Never a structure with micromaps: see inputsOf.
        // And never a size read back for a build before its last change of room and geometry (a split).
        if (!blas.live || blas.generation != due.generation || blas.serial != due.serial || blas.compacted || blas.micromaps || due.size >= blas.size) continue;

        const auto to = blasAllocator.allocate(due.size, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        if (!to) continue;

        compactions.push_back({due.id, blas.offset, *to});
        releases.push_back({signalValue, Pool::Structures, blas.offset, blas.size});
        tally.compactedFrom += blas.size;
        tally.compactedTo += due.size;
        ++tally.compactedThisFrame;
        blas.offset = *to;
        blas.size = due.size;
        blas.compacted = true;
    }

    compactionsDue.clear();
}

void Scene::recordWrites(ID3D12GraphicsCommandList4& list)
{
    // The pool: the frame's uploads, then writable for the kernels that make geometry.
    auto barrier = transition(geometryPool.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        poolUploads.empty() ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST);
    list.ResourceBarrier(1, &barrier);
    if (!poolUploads.empty())
    {
        for (const auto& upload : poolUploads)
            list.CopyBufferRegion(geometryPool.Get(), upload.poolOffset, uploads[slot].Get(), upload.uploadOffset, upload.bytes);

        barrier = transition(geometryPool.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list.ResourceBarrier(1, &barrier);
    }

    // The micromap pool: the frame's records, descriptors and triangles' blocks, then writable for the bake.
    if (!bakes.empty())
    {
        barrier = transition(micromaps.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list.ResourceBarrier(1, &barrier);
        for (const auto& upload : micromapUploads)
            list.CopyBufferRegion(micromaps.Get(), upload.poolOffset, uploads[slot].Get(), upload.uploadOffset, upload.bytes);

        barrier = transition(micromaps.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list.ResourceBarrier(1, &barrier);
    }

    // The table's rows written since the last frame, in runs.
    if (dirtyRows.empty()) return;

    std::sort(dirtyRows.begin(), dirtyRows.end());
    dirtyRows.erase(std::unique(dirtyRows.begin(), dirtyRows.end()), dirtyRows.end());
    barrier = transition(table.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list.ResourceBarrier(1, &barrier);
    std::size_t written{};
    while (written < dirtyRows.size())
    {
        std::size_t end = written + 1;
        while (end < dirtyRows.size() && dirtyRows[end] == dirtyRows[end - 1] + 1) ++end;

        const auto first = dirtyRows[written];
        const auto bytes = (end - written) * sizeof(GIGeometry);
        const auto at = reserveUpload(bytes);
        if (at == UploadBytes) break;

        std::memcpy(uploadData[slot] + at, &rows[first], bytes);
        list.CopyBufferRegion(table.Get(), std::uint64_t{first} * sizeof(GIGeometry), uploads[slot].Get(), at, bytes);
        written = end;
    }

    // What did not fit goes next frame.
    dirtyRows.erase(dirtyRows.begin(), dirtyRows.begin() + static_cast<std::ptrdiff_t>(written));
    barrier = transition(table.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list.ResourceBarrier(1, &barrier);
}

// PHANTOM_FX_D3D12_DRED labels each build, so a device hang's breadcrumbs name the build that never
// finished (the markers are DRED's breadcrumb contexts).
static void mark(ID3D12GraphicsCommandList4& list, const std::string& text)
{
    static const bool on = GetEnvironmentVariableA("PHANTOM_FX_D3D12_DRED", nullptr, 0) != 0;
    if (!on) return;
    const std::wstring wide(text.begin(), text.end());
    list.SetMarker(1, wide.c_str(), static_cast<UINT>((wide.size() + 1) * sizeof(wchar_t)));
}

void Scene::recordBottomLevels(ID3D12GraphicsCommandList4& list)
{
    D3D12_RESOURCE_BARRIER readable[]{uavBarrier(), transition(geometryPool.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list.ResourceBarrier(static_cast<UINT>(std::size(readable)), readable);

    // The baked micromaps' arrays, which the bottom levels built after link to.
    // The opacity split's classifications: what the bake found of each block, into the slot's readback.
    if (!bakes.empty() && !splitCopies.empty())
    {
        auto barrier = transition(micromaps.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list.ResourceBarrier(1, &barrier);
        for (const auto& copy : splitCopies)
            list.CopyBufferRegion(splitReadback[slot].Get(), copy.uploadOffset, micromaps.Get(), copy.poolOffset, copy.bytes);
        barrier = transition(micromaps.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list.ResourceBarrier(1, &barrier);
    }
    else if (!bakes.empty())
    {
        const auto baked = transition(micromaps.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list.ResourceBarrier(1, &baked);
    }
    for (const auto& build : arrayBuilds)
    {
        const auto& blas = structures[build.id];
        if (!blas.live || blas.generation != build.generation || !blas.micromaps) continue;

        D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC array{};
        array.NumOmmHistogramEntries = static_cast<UINT>(build.histogram.size());
        array.pOmmHistogram = build.histogram.data();
        array.InputBuffer = micromapAddress(build.data);
        array.PerOmmDescs = {micromapAddress(build.descs), sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC)};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
        desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        desc.Inputs.NumDescs = 1;
        desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        desc.Inputs.pOpacityMicromapArrayDesc = &array;
        desc.DestAccelerationStructureData = blasPool->GetGPUVirtualAddress() + blas.micromaps->arrayOffset;
        desc.ScratchAccelerationStructureData = micromapScratch->GetGPUVirtualAddress() + build.scratch;
        if (trace::on()) trace::add("build micromap array " + std::to_string(build.id) + " " + std::to_string(blas.micromaps->blocks) + " blocks");
        mark(list, "micromap array " + std::to_string(build.id) + " " + std::to_string(blas.micromaps->blocks) + " blocks");
        list.BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    }
    if (!arrayBuilds.empty())
    {
        const auto arrays = uavBarrier();
        list.ResourceBarrier(1, &arrays);
    }

    bool postbuild{};
    for (const auto& build : builds)
    {
        // Gone since it was planned: nothing reads what the build would write.
        const auto& blas = structures[build.id];
        if (!blas.live || blas.generation != build.generation) continue;

        if (trace::on())
            trace::add(std::string("build structure ") + std::to_string(build.id) + " " + std::to_string(blas.triangles) + " triangles"
                + (blas.micromaps ? " linked" : ""));
        mark(list, "structure " + std::to_string(build.id) + " " + std::to_string(blas.triangles) + " triangles" + (build.update ? " update" : "")
            + (blas.micromaps ? " linked" : "") + (blas.pendingLink ? " pending" : ""));
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs = inputsOf(blas, build.update);
        desc.DestAccelerationStructureData = address(blas);
        desc.SourceAccelerationStructureData = build.update ? address(blas) : 0;
        desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress() + build.scratch;
        if (blas.use == Use::Static && !blas.micromaps && build.postbuild != NoPostbuild)
        {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{postbuildSizes->GetGPUVirtualAddress() + std::uint64_t{build.postbuild} * 8,
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE};
            list.BuildRaytracingAccelerationStructure(&desc, 1, &info);
            postbuild = true;
        }
        else
            list.BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

        ++tally.builtThisFrame;
        tally.builtTriangles += blas.triangles;
    }

    const auto all = uavBarrier();
    list.ResourceBarrier(1, &all);
    if (postbuild)
    {
        auto barrier = transition(postbuildSizes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list.ResourceBarrier(1, &barrier);
        list.CopyBufferRegion(postbuildReadback[slot].Get(), 0, postbuildSizes.Get(), 0, builtInSlot[slot].size() * 8);
        barrier = transition(postbuildSizes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list.ResourceBarrier(1, &barrier);
    }

    for (const auto& compaction : compactions)
    {
        if (trace::on()) trace::add("compact structure " + std::to_string(compaction.id));
        mark(list, "compact " + std::to_string(compaction.id));
        list.CopyRaytracingAccelerationStructure(blasPool->GetGPUVirtualAddress() + compaction.to, blasPool->GetGPUVirtualAddress() + compaction.from,
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
    }

    if (!compactions.empty()) list.ResourceBarrier(1, &all);
}

void Scene::recordInstances(ID3D12GraphicsCommandList4& list)
{
    auto barrier = transition(instanceDescs.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list.ResourceBarrier(1, &barrier);
    if (!instances.empty())
    {
        const auto bytes = instances.size() * sizeof(GIInstanceDesc);
        const auto at = reserveUpload(bytes);
        if (at == UploadBytes) throw std::runtime_error("The scene's upload buffer has no room for the frame's instances.");

        std::memcpy(uploadData[slot] + at, instances.data(), bytes);
        list.CopyBufferRegion(instanceDescs.Get(), 0, uploads[slot].Get(), at, bytes);
    }

    barrier = transition(instanceDescs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list.ResourceBarrier(1, &barrier);
}

void Scene::recordTopLevel(ID3D12GraphicsCommandList4& list)
{
    D3D12_RESOURCE_BARRIER ready[]{uavBarrier(), transition(instanceDescs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list.ResourceBarrier(static_cast<UINT>(std::size(ready)), ready);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC top{};
    top.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    top.Inputs.NumDescs = instanceCount();
    top.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.Inputs.InstanceDescs = instanceDescs->GetGPUVirtualAddress();
    top.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    top.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    if (trace::on()) trace::add("build top level " + std::to_string(instanceCount()) + " instances");
    mark(list, "top level " + std::to_string(instanceCount()) + " instances");
    list.BuildRaytracingAccelerationStructure(&top, 0, nullptr);

    const auto all = uavBarrier();
    list.ResourceBarrier(1, &all);
}

std::optional<std::uint64_t> Scene::allocate(std::uint64_t bytes)
{
    return poolAllocator.allocate(aligned(bytes, 16), 16);
}

void Scene::release(std::uint64_t offset, std::uint64_t bytes)
{
    releases.push_back({signalValue, Pool::Geometry, offset, aligned(bytes, 16)});
}

bool Scene::upload(std::uint64_t offset, std::span<const std::uint8_t> data)
{
    if (geometryUploaded + data.size() > GeometryUploadBytes) return false;

    const auto at = reserveUpload(data.size());
    if (at == UploadBytes) return false;

    std::memcpy(uploadData[slot] + at, data.data(), data.size());
    poolUploads.push_back({offset, at, data.size()});
    geometryUploaded += data.size();
    return true;
}

std::uint64_t Scene::uploadRoom() const
{
    return GeometryUploadBytes - geometryUploaded;
}

std::optional<BlasId> Scene::createBlas(Use use, const std::vector<Geometry>& geometries)
{
    if (geometries.empty()) return std::nullopt;

    Blas blas;
    blas.use = use;
    const auto poolAddress = geometryPool->GetGPUVirtualAddress();
    for (const auto& geometry : geometries)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC desc{};
        desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        // Hair dims a ray once for each layer it crosses (PassHair), so a ray must meet each of its triangles once.
        if (!(geometry.flags & GI_GEOMETRY_CUT_OUT)) desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        else if (geometry.flags & GI_GEOMETRY_HAIR) desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
        desc.Triangles.VertexBuffer = {poolAddress + geometry.vertexOffset, 16};
        desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        desc.Triangles.VertexCount = geometry.vertexCount;
        if (geometry.indexCount)
        {
            desc.Triangles.IndexBuffer = poolAddress + geometry.indexOffset;
            desc.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
            desc.Triangles.IndexCount = geometry.indexCount;
        }

        blas.geometries.push_back(desc);
        blas.triangles += (geometry.indexCount ? geometry.indexCount : geometry.vertexCount) / 3;
    }

    // A static structure's cut-out meshes keep what their micromaps (or their opacity split) are baked
    // from once their textures come.
    if ((micromapsOn || splitsOn) && use == Use::Static)
        for (std::uint32_t i = 0; i < geometries.size(); ++i)
        {
            const auto& geometry = geometries[i];
            const bool takes = (geometry.flags & GI_GEOMETRY_CUT_OUT) && !(geometry.flags & GI_GEOMETRY_HAIR) && !geometry.uvs.empty()
                && geometry.indices.size() == geometry.indexCount && geometry.indexCount;
            if (!takes) continue;

            blas.sources.resize(geometries.size());
            blas.sources[i] = {{geometry.uvs.begin(), geometry.uvs.end()}, {geometry.indices.begin(), geometry.indices.end()}};
            blas.micromapsDirty = true;
        }

    auto inputs = inputsOf(blas, false);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    // A row more for each mesh the opacity split may give an opaque part.
    blas.meshes = static_cast<std::uint32_t>(geometries.size());
    blas.rowCount = blas.meshes;
    if (splitsOn)
    {
        for (const auto& source : blas.sources)
            if (!source.uvs.empty()) ++blas.rowCount;
        blas.splitRows.assign(blas.meshes, NoRow);
        blas.meshSplit.assign(blas.meshes, false);
    }

    const auto first = rowAllocator.allocate(blas.rowCount);
    if (!first) return std::nullopt;

    const auto offset = blasAllocator.allocate(prebuild.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    if (!offset)
    {
        rowAllocator.release(*first, blas.rowCount);
        return std::nullopt;
    }

    blas.live = true;
    blas.firstGeometry = static_cast<std::uint32_t>(*first);
    blas.offset = *offset;
    blas.size = prebuild.ResultDataMaxSizeInBytes;
    blas.scratch = aligned(prebuild.ScratchDataSizeInBytes, 256);
    blas.updateScratch = aligned(prebuild.UpdateScratchDataSizeInBytes, 256);

    for (std::uint32_t i = 0; i < geometries.size(); ++i)
    {
        const auto& geometry = geometries[i];
        GIGeometry row{static_cast<std::uint32_t>(geometry.vertexOffset), geometry.indexCount ? static_cast<std::uint32_t>(geometry.indexOffset) : GI_NO_INDICES,
            geometry.flags, geometry.texture, geometry.alphaThreshold, geometry.specularTexture, geometry.normalTexture, geometry.materialIndex,
            {geometry.emission[0], geometry.emission[1], geometry.emission[2], 0}, static_cast<std::uint32_t>(geometry.normalOffset), {}};
        writeRow(blas.firstGeometry + i, row);
        newRows.push_back(blas.firstGeometry + i);
    }

    BlasId id;
    if (!freeIds.empty())
    {
        id = freeIds.back();
        freeIds.pop_back();
        blas.generation = structures[id].generation + 1;
    }
    else
    {
        id = static_cast<BlasId>(structures.size());
        structures.emplace_back();
    }

    if (use == Use::Static)
    {
        blas.queued = true;
        buildQueue.push_back(id);
    }

    structures[id] = std::move(blas);
    return id;
}

void Scene::destroyBlas(BlasId id)
{
    auto& blas = structures[id];
    if (!blas.live) return;

    releases.push_back({signalValue, Pool::Structures, blas.offset, blas.size});
    releases.push_back({signalValue, Pool::Rows, blas.firstGeometry, blas.rowCount});
    if (blas.micromaps) releaseMicromaps(*blas.micromaps);
    for (const auto& [at, bytes] : blas.splitIndices) release(at, bytes);

    const auto generation = blas.generation;
    blas = {};
    blas.generation = generation;
    freeIds.push_back(id);
}

bool Scene::rebuild(BlasId id)
{
    auto& blas = structures[id];
    if (scratchUsed + blas.scratch > tlasScratch + BuildScratchBytes)
    {
        blas.built = false;
        return false;
    }

    builds.push_back({id, blas.generation, false, scratchUsed, NoPostbuild});
    scratchUsed += blas.scratch;
    blas.built = true;
    return true;
}

bool Scene::refit(BlasId id)
{
    auto& blas = structures[id];
    if (!blas.built) return rebuild(id);
    if (scratchUsed + blas.updateScratch > tlasScratch + BuildScratchBytes) return false;

    builds.push_back({id, blas.generation, true, scratchUsed, NoPostbuild});
    scratchUsed += blas.updateScratch;
    return true;
}

void Scene::setMaterial(BlasId id, std::uint32_t geometry, const scene_description::Material& material)
{
    auto& blas = structures[id];
    if (!blas.live || geometry >= blas.meshes) return;

    auto row = rows[blas.firstGeometry + geometry];
    const auto before = row;
    row.flags = (row.flags & GI_GEOMETRY_CUT_OUT) | (material.flags & ~GI_GEOMETRY_CUT_OUT);
    row.texture = material.texture;
    row.alphaThreshold = material.alphaThreshold;
    row.specularTexture = material.specularTexture;
    row.normalTexture = material.normalTexture;
    writeRow(blas.firstGeometry + geometry, row);
    // The mesh's opaque part, if split, is the same material over its own triangles.
    if (geometry < blas.splitRows.size() && blas.splitRows[geometry] != NoRow)
    {
        auto opaqueRow = row;
        opaqueRow.indexOffset = rows[blas.splitRows[geometry]].indexOffset;
        writeRow(blas.splitRows[geometry], opaqueRow);
    }

    // A structure that carries micromaps keeps them: they were baked from the 512-pixel copy the rays
    // sample, so the game's finer mips arriving (a new slot, the same picture) change nothing they see,
    // and relinking would rebuild the structure for no quality gain. Only one that had no texture yet
    // asks again.
    const bool hasSource = geometry < blas.sources.size() && !blas.sources[geometry].uvs.empty();
    const bool takes = (row.flags & GI_GEOMETRY_CUT_OUT) && !(row.flags & GI_GEOMETRY_HAIR) && row.texture;
    const bool split = geometry < blas.meshSplit.size() && blas.meshSplit[geometry];
    if (hasSource && takes && !blas.micromaps && !split) blas.micromapsDirty = true;
}

bool Scene::ready(BlasId id) const
{
    const auto& blas = structures[id];
    if (!blas.live) return false;

    return blas.built;
}

std::vector<std::uint32_t> Scene::takeNewGeometries()
{
    return std::exchange(newRows, {});
}

std::optional<std::uint32_t> Scene::addInstance(BlasId id, const float* transform, std::uint32_t kind)
{
    if (instances.size() >= MaxInstances || !ready(id)) return std::nullopt;

    const auto& blas = structures[id];
    GIInstanceDesc desc{};
    for (int row = 0; row < 3; ++row)
    {
        float* to = &desc.transform[row].x;
        for (int column = 0; column < 4; ++column) to[column] = transform ? transform[row * 4 + column] : (row == column ? 1.0f : 0.0f);
    }

    // Only the terrain and the sea are one-sided (GI_RAY_FLAGS); the game's meshes are often open shells. The
    // sea's rays from the waves' troughs below its plane pass its back.
    std::uint32_t flags = (kind & (GI_INSTANCE_TERRAIN | GI_INSTANCE_SEA)) ? 0 : D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    // PHANTOM_FX_OMM_DISABLE_INSTANCES leaves the micromaps linked and built but unused at traversal:
    // tells a fault in building and releasing them from one in reading them. Read once: this runs for
    // every instance of every frame.
    static const bool disableMicromaps = GetEnvironmentVariableA("PHANTOM_FX_OMM_DISABLE_INSTANCES", nullptr, 0) != 0;
    if (disableMicromaps) flags |= D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS;
    desc.idAndMask = (kind & 0xFFFFFFu) | (GI_MASK_SCENE << 24);
    desc.hitGroupAndFlags = (blas.firstGeometry & 0xFFFFFFu) | (flags << 24);
    const auto blasAddress = address(blas);
    desc.blas = {static_cast<std::uint32_t>(blasAddress), static_cast<std::uint32_t>(blasAddress >> 32)};
    instances.push_back(desc);
    return static_cast<std::uint32_t>(instances.size() - 1);
}

std::optional<std::uint32_t> Scene::openTexture(const SceneTexture& texture)
{
    if (freeTextures.empty()) return std::nullopt;

    const auto textureSlot = freeTextures.back();
    auto& opened = textures[textureSlot];
    check(opened.open(device, texture.texture), "Opening one of the game's textures");
    freeTextures.pop_back();
    writeTexture(textureSlot, opened.resource.Get(), texture.viewFormat, texture.mips);
    D3D11_TEXTURE2D_DESC desc{};
    texture.texture->GetDesc(&desc);
    textureSizes[textureSlot] = {desc.Width, desc.Height};

    return textureSlot;
}

void Scene::closeTexture(std::uint32_t textureSlot)
{
    closedTextures.push_back({signalValue, textureSlot, std::move(textures[textureSlot].resource)});
    textures[textureSlot] = {};
}

Scene::Stats Scene::stats() const
{
    auto result = tally;
    result.structures = result.compacted = 0;
    for (const auto& blas : structures)
        if (blas.live)
        {
            ++result.structures;
            if (blas.compacted) ++result.compacted;
        }

    result.poolUsed = poolAllocator.usedSize();
    result.poolCapacity = poolAllocator.capacity();
    result.blasUsed = blasAllocator.usedSize();
    result.blasCapacity = blasAllocator.capacity();
    result.geometriesUsed = static_cast<std::uint32_t>(rowAllocator.usedSize());
    result.micromapStructures = result.micromapBlocks = 0;
    result.micromapTriangles = result.micromapBlockBytes = 0;
    for (const auto& blas : structures)
        if (blas.live && blas.micromaps)
        {
            ++result.micromapStructures;
            result.micromapBlocks += blas.micromaps->blocks;
            result.micromapTriangles += blas.micromaps->triangleCount;
            for (std::size_t level = 0; level < result.micromapLevels.size(); ++level)
            {
                result.micromapLevels[level] += blas.micromaps->levels[level];
                result.micromapBlockBytes += std::uint64_t{blas.micromaps->levels[level]} * omm_layout::blockBytes(static_cast<std::uint32_t>(level));
            }
        }
    result.micromapLinks = micromapLinks;
    result.micromapRelinks = micromapRelinks;
    result.micromapPoolUsed = micromapAllocator.usedSize();
    result.splitStructures = splitStructures;
    result.splitOpaque = splitOpaque;
    result.splitTransparent = splitTransparent;
    result.splitMixed = splitMixed;
    return result;
}

void Scene::enableMicromaps()
{
    if (micromapsOn) return;

    micromaps = createBuffer(device, MicromapPoolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the micromap pool");
    micromapScratch = createStorage(device, MicromapScratchBytes, "Creating the micromap scratch");
    micromapsOn = true;
}

void Scene::enableOpacitySplit()
{
    if (micromapsOn || splitsOn) return;

    // The bake's room: the micromap pool, holding a classification's records, blocks and states for a frame.
    micromaps = createBuffer(device, MicromapPoolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the opacity split's pool");
    for (auto& readback : splitReadback)
        readback = createBuffer(device, SplitReadbackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
            "Creating the opacity split's readback");
    splitsOn = true;
}

bool Scene::classify(BlasId id)
{
    auto& blas = structures[id];

    // The meshes not split yet whose textures have come; the rest wait for theirs (setMaterial).
    std::vector<omm_layout::Layout> layouts;
    std::vector<std::uint32_t> meshes;
    for (std::uint32_t i = 0; i < blas.sources.size(); ++i)
    {
        const auto& source = blas.sources[i];
        const auto& row = rows[blas.firstGeometry + i];
        if (source.uvs.empty() || blas.meshSplit[i] || !omm_layout::takesMicromaps(row.flags) || !row.texture || row.texture > GI_MAX_TEXTURES) continue;

        const auto size = textureSizes[row.texture - 1];
        if (!size[0] || !size[1]) continue;

        if (auto layout = omm_layout::layOut({source.uvs, source.indices, size[0], size[1], MicromapBytesPerMesh}))
        {
            layouts.push_back(std::move(*layout));
            meshes.push_back(i);
        }
    }

    blas.micromapsDirty = false;
    if (layouts.empty()) return true;

    // The pool's room for the bake: the blocks' states it writes, its records, and what it finds of each
    // block, which is all that comes back.
    const auto array = omm_layout::combine(layouts);
    const auto blocks = static_cast<std::uint32_t>(array.blocks.size());
    const std::uint64_t recordsAt = aligned(array.bytes, 16), statesAt = aligned(recordsAt + std::uint64_t{sizeof(gi::GIOmmBlock)} * blocks, 16),
                        poolBytes = aligned(statesAt + 4ull * blocks, 16);
    const auto recordBytes = std::uint64_t{sizeof(gi::GIOmmBlock)} * blocks;
    std::optional<std::uint64_t> poolOffset;
    if (aligned(uploadUsed, 16) + recordBytes > UploadBytes || splitReadbackUsed + 4ull * blocks > SplitReadbackBytes
        || !(poolOffset = micromapAllocator.allocate(poolBytes, 16)))
    {
        blas.micromapsDirty = true;
        return false;
    }

    std::vector<gi::GIOmmBlock> records(blocks);
    for (std::uint32_t b = 0; b < blocks; ++b)
    {
        const auto& block = array.blocks[b];
        const auto& row = rows[blas.firstGeometry + meshes[array.blockMesh[b]]];
        records[b] = {block.uvs[0], block.uvs[1], block.uvs[2], block.level, row.texture, block.byteOffset, row.alphaThreshold, 0};
    }
    const auto at = reserveUpload(recordBytes);
    std::memcpy(uploadData[slot] + at, records.data(), recordBytes);
    micromapUploads.push_back({*poolOffset + recordsAt, at, recordBytes});

    const auto pool = [&](std::uint64_t offset) { return static_cast<std::uint32_t>(*poolOffset + offset); };
    bakes.push_back({pool(recordsAt), blocks, pool(statesAt), pool(0), 0, 0, 0});
    splitCopies.push_back({*poolOffset + statesAt, splitReadbackUsed, 4ull * blocks});
    classifying[slot].push_back({id, blas.generation, splitReadbackUsed, blocks, std::move(meshes), array.firstTriangle, array.triangleBlocks});
    splitReadbackUsed += aligned(4ull * blocks, 16);
    releases.push_back({signalValue, Pool::Micromaps, *poolOffset, poolBytes});
    blas.classifying = true;
    return true;
}

Scene::SplitResult Scene::applySplit(SplitDue& due, std::uint32_t postbuild)
{
    auto& blas = structures[due.id];
    if (!blas.live || blas.generation != due.generation) return SplitResult::Dropped;
    if (!blas.built || blas.queued) return SplitResult::Later;

    // The meshes worth splitting: an opaque part, or triangles to drop. A mesh whose triangles are all
    // transparent keeps one that is never met (three times its first corner), flagged opaque.
    struct Part
    {
        std::uint32_t mesh{};
        const std::vector<std::uint16_t>* opaque{};
        const std::vector<std::uint16_t>* mixed{};
    };
    std::vector<Part> parts;
    std::vector<std::vector<std::uint16_t>> empties;
    empties.reserve(due.parts.size());
    std::uint64_t uploadBytes{};
    std::uint32_t appended{};
    for (std::size_t k = 0; k < due.parts.size(); ++k)
    {
        auto& split = due.parts[k];
        const auto mesh = due.meshes[k];
        if (split.opaque.empty() && !split.transparent) continue;
        if (split.opaque.empty() && split.mixed.empty())
        {
            const auto first = blas.sources[mesh].indices.front();
            split.opaque = {first, first, first};
        }
        parts.push_back({mesh, &split.opaque, &split.mixed});
        uploadBytes += aligned(split.opaque.size() * 2, 16) + aligned(split.mixed.size() * 2, 16);
        if (!split.opaque.empty() && !split.mixed.empty()) ++appended;
    }
    const auto markDone = [&] {
        for (const auto mesh : due.meshes) blas.meshSplit[mesh] = true;
    };
    if (parts.empty())
    {
        markDone();
        return SplitResult::Dropped;
    }
    if (blas.geometries.size() + appended > blas.rowCount) return SplitResult::Dropped;
    if (uploadBytes + 16 * parts.size() * 2 > uploadRoom()) return SplitResult::Later;

    // The geometries as they will be, their index buffers set once the lists have their room.
    auto geometries = blas.geometries;
    for (const auto& part : parts)
    {
        auto& tested = geometries[part.mesh];
        if (!part.mixed->empty())
        {
            tested.Triangles.IndexCount = static_cast<UINT>(part.mixed->size());
            if (!part.opaque->empty())
            {
                auto opaque = tested;
                opaque.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                opaque.Triangles.IndexCount = static_cast<UINT>(part.opaque->size());
                geometries.push_back(opaque);
            }
        }
        else
        {
            tested.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            tested.Triangles.IndexCount = static_cast<UINT>(part.opaque->size());
        }
    }

    auto inputs = inputsOf(blas, false);
    inputs.NumDescs = static_cast<UINT>(geometries.size());
    inputs.pGeometryDescs = geometries.data();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    const auto blasScratch = aligned(prebuild.ScratchDataSizeInBytes, 256);
    if (scratchUsed + blasScratch > tlasScratch + BuildScratchBytes) return SplitResult::Later;
    const auto offset = blasAllocator.allocate(prebuild.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    if (!offset) return SplitResult::Later;

    // The parts' index lists in the geometry pool, all placed and uploaded (this frame, before the build)
    // before any row changes: a list without room leaves the structure as it is, the placed ones
    // returning with the frame.
    std::vector<std::array<std::uint64_t, 2>> placed;
    const auto place = [&](const std::vector<std::uint16_t>& list) -> std::uint64_t {
        if (list.empty()) return NoList;
        const auto bytes = list.size() * 2;
        const auto at = allocate(bytes);
        if (!at) return NoList;
        placed.push_back({*at, bytes});
        return upload(*at, {reinterpret_cast<const std::uint8_t*>(list.data()), bytes}) ? *at : NoList;
    };
    std::vector<std::array<std::uint64_t, 2>> lists; // Each part's mixed and opaque lists' offsets
    for (const auto& part : parts)
    {
        lists.push_back({place(*part.mixed), place(*part.opaque)});
        if ((!part.mixed->empty() && lists.back()[0] == NoList) || (!part.opaque->empty() && lists.back()[1] == NoList))
        {
            blasAllocator.release(*offset, prebuild.ResultDataMaxSizeInBytes);
            for (const auto& [at, bytes] : placed) release(at, bytes);
            return SplitResult::Later;
        }
    }

    const auto poolAddress = geometryPool->GetGPUVirtualAddress();
    auto next = static_cast<std::uint32_t>(blas.geometries.size());
    for (std::size_t p = 0; p < parts.size(); ++p)
    {
        const auto& part = parts[p];
        const auto [mixedAt, opaqueAt] = lists[p];
        auto row = rows[blas.firstGeometry + part.mesh];
        const auto tested = mixedAt != NoList ? mixedAt : opaqueAt;
        geometries[part.mesh].Triangles.IndexBuffer = poolAddress + tested;
        row.indexOffset = static_cast<std::uint32_t>(tested);
        writeRow(blas.firstGeometry + part.mesh, row);
        if (mixedAt != NoList && opaqueAt != NoList)
        {
            geometries[next].Triangles.IndexBuffer = poolAddress + opaqueAt;
            auto opaqueRow = row;
            opaqueRow.indexOffset = static_cast<std::uint32_t>(opaqueAt);
            writeRow(blas.firstGeometry + next, opaqueRow);
            newRows.push_back(blas.firstGeometry + next);
            blas.splitRows[part.mesh] = blas.firstGeometry + next;
            ++next;
        }
    }
    blas.splitIndices.insert(blas.splitIndices.end(), placed.begin(), placed.end());

    for (std::size_t k = 0; k < due.parts.size(); ++k)
    {
        const auto& split = due.parts[k];
        splitOpaque += split.opaque.size() / 3;
        splitTransparent += split.transparent;
        splitMixed += split.mixed.size() / 3;
    }
    markDone();
    ++splitStructures;

    // Built again in new room: the frames still in flight trace the old.
    releases.push_back({signalValue, Pool::Structures, blas.offset, blas.size});
    blas.geometries = std::move(geometries);
    blas.offset = *offset;
    blas.size = prebuild.ResultDataMaxSizeInBytes;
    blas.scratch = blasScratch;
    blas.updateScratch = aligned(prebuild.UpdateScratchDataSizeInBytes, 256);
    blas.compacted = false;
    ++blas.serial;
    builds.push_back({due.id, blas.generation, false, scratchUsed, postbuild});
    scratchUsed += blasScratch;
    return SplitResult::Built;
}

bool Scene::prepareMicromaps(BlasId id)
{
    auto& blas = structures[id];

    // Each source whose geometry takes micromaps with its texture as it is now.
    std::vector<omm_layout::Layout> layouts;
    std::vector<std::uint32_t> linkedGeometries;
    for (std::uint32_t i = 0; i < blas.sources.size(); ++i)
    {
        const auto& source = blas.sources[i];
        const auto& row = rows[blas.firstGeometry + i];
        if (source.uvs.empty() || !omm_layout::takesMicromaps(row.flags) || !row.texture || row.texture > GI_MAX_TEXTURES) continue;

        const auto size = textureSizes[row.texture - 1];
        if (!size[0] || !size[1]) continue;

        if (auto layout = omm_layout::layOut({source.uvs, source.indices, size[0], size[1], MicromapBytesPerMesh}))
        {
            layouts.push_back(std::move(*layout));
            linkedGeometries.push_back(i);
        }
    }

    if (layouts.empty() && !blas.micromaps) return true;

    // The pool's room: the array's input, its descriptors, the bake's records, the triangles' blocks,
    // the blocks' states, the linkage's index buffer.
    const auto array = omm_layout::combine(layouts);
    const auto blocks = static_cast<std::uint32_t>(array.blocks.size());
    const auto triangles = static_cast<std::uint32_t>(array.triangleBlocks.size());
    const std::uint64_t descsAt = aligned(array.bytes, 16), recordsAt = aligned(descsAt + 8ull * blocks, 16),
                        trianglesAt = recordsAt + std::uint64_t{sizeof(gi::GIOmmBlock)} * blocks, statesAt = aligned(trianglesAt + 4ull * triangles, 16),
                        indicesAt = aligned(statesAt + 4ull * blocks, 16),
                        poolBytes = blocks ? aligned(indicesAt + 4ull * triangles, MicromapArrayAlignment) : 0;
    const std::uint64_t uploadBytes = (8ull + sizeof(gi::GIOmmBlock)) * blocks + 4ull * triangles + 48;
    const auto refuse = [&](const char* why) {
        log(std::string("Opacity micromaps: structure ") + std::to_string(id) + " not linked, " + why + ".");
        return false;
    };
    if (blocks && aligned(uploadUsed, 16) + uploadBytes > UploadBytes) return refuse("the frame's upload has no room");

    std::optional<std::uint64_t> poolOffset;
    if (blocks && !(poolOffset = micromapAllocator.allocate(poolBytes, MicromapArrayAlignment))) return refuse("the micromap pool has no room");

    LinkedMicromaps linked;
    linked.blocks = blocks;
    std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> histogram;
    std::uint64_t arrayScratch{};
    if (blocks)
    {
        linked.poolOffset = *poolOffset;
        linked.poolBytes = poolBytes;
        for (std::uint32_t level = 0; level < array.histogram.size(); ++level)
            if (array.histogram[level]) histogram.push_back({array.histogram[level], level, D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE});

        D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC arrayDesc{};
        arrayDesc.NumOmmHistogramEntries = static_cast<UINT>(histogram.size());
        arrayDesc.pOmmHistogram = histogram.data();
        arrayDesc.InputBuffer = micromapAddress(*poolOffset);
        arrayDesc.PerOmmDescs = {micromapAddress(*poolOffset + descsAt), sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC)};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.NumDescs = 1;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.pOpacityMicromapArrayDesc = &arrayDesc;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
        const auto arrayOffset = prebuild.ResultDataMaxSizeInBytes
            ? blasAllocator.allocate(prebuild.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT)
            : std::nullopt;
        if (!arrayOffset)
        {
            micromapAllocator.release(*poolOffset, poolBytes);
            return refuse("the structures' pool has no room for the array");
        }

        linked.arrayOffset = *arrayOffset;
        linked.arraySize = prebuild.ResultDataMaxSizeInBytes;
        arrayScratch = aligned(prebuild.ScratchDataSizeInBytes, 256);
    }

    // The geometries as built with their micromaps; the vectors are sized once, so the pointers hold.
    linked.triangles.reserve(layouts.size());
    linked.linkages.reserve(layouts.size());
    linked.geometries = blas.geometries;
    for (std::uint32_t k = 0; k < layouts.size(); ++k)
    {
        auto& geometry = linked.geometries[linkedGeometries[k]];
        linked.triangles.push_back(geometry.Triangles);
        D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC linkage{};
        linkage.OpacityMicromapIndexBuffer = {micromapAddress(linked.poolOffset + indicesAt + 4ull * array.firstTriangle[k]), 4};
        linkage.OpacityMicromapIndexFormat = DXGI_FORMAT_R32_UINT;
        linkage.OpacityMicromapArray = blasPool->GetGPUVirtualAddress() + linked.arrayOffset;
        linked.linkages.push_back(linkage);

        // PHANTOM_FX_OMM_NOLINK builds the array and leaves the structure unlinked: tells a fault in the
        // array's build from one in a structure that links it.
        static const bool noLink = GetEnvironmentVariableA("PHANTOM_FX_OMM_NOLINK", nullptr, 0) != 0;
        if (noLink) continue;

        const auto flags = geometry.Flags;
        geometry = {};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
        geometry.Flags = flags;
        geometry.OmmTriangles = {&linked.triangles.back(), &linked.linkages.back()};
    }

    if (blocks && micromapScratchUsed + arrayScratch > MicromapScratchBytes)
    {
        blasAllocator.release(linked.arrayOffset, linked.arraySize);
        micromapAllocator.release(linked.poolOffset, poolBytes);
        return refuse("the micromap scratch has no room");
    }

    // Committed: the pool's inputs uploaded, the bake and the array's build recorded.
    if (blocks)
    {
        std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_DESC> descs(blocks);
        std::vector<gi::GIOmmBlock> records(blocks);
        for (std::uint32_t b = 0; b < blocks; ++b)
        {
            const auto& block = array.blocks[b];
            descs[b].ByteOffset = block.byteOffset;
            descs[b].SubdivisionLevel = block.level;
            descs[b].Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE;
            const auto& row = rows[blas.firstGeometry + linkedGeometries[array.blockMesh[b]]];
            records[b] = {block.uvs[0], block.uvs[1], block.uvs[2], block.level, row.texture, block.byteOffset, row.alphaThreshold, 0};
        }

        // What the array build is handed, for the first few links: the numbers either add up or name the bug.
        static int dumps{};
        if (dumps++ < 3)
        {
            std::string levels;
            for (const auto& entry : histogram)
                levels += " L" + std::to_string(entry.SubdivisionLevel) + "x" + std::to_string(entry.Count) + "f" + std::to_string(static_cast<int>(entry.Format));
            std::uint64_t largest{};
            for (const auto& block : array.blocks) largest = std::max(largest, std::uint64_t{block.byteOffset} + omm_layout::blockBytes(block.level));
            log("Opacity micromaps: array for structure " + std::to_string(id) + ": " + std::to_string(blocks) + " blocks, " + std::to_string(triangles)
                + " triangles, input " + std::to_string(array.bytes) + " B, largest end " + std::to_string(largest) + " B, descs " + std::to_string(descsAt)
                + " stride " + std::to_string(sizeof(descs[0])) + ", histogram" + levels + ", first desc " + std::to_string(descs[0].ByteOffset) + " L"
                + std::to_string(descs[0].SubdivisionLevel) + " f" + std::to_string(static_cast<int>(descs[0].Format)) + ", last desc "
                + std::to_string(descs.back().ByteOffset) + " L" + std::to_string(descs.back().SubdivisionLevel));
        }

        const auto stage = [&](std::uint64_t poolAt, const void* data, std::uint64_t bytes) {
            const auto at = reserveUpload(bytes);
            std::memcpy(uploadData[slot] + at, data, bytes);
            micromapUploads.push_back({linked.poolOffset + poolAt, at, bytes});
        };
        stage(descsAt, descs.data(), descs.size() * sizeof(descs[0]));
        stage(recordsAt, records.data(), records.size() * sizeof(records[0]));
        stage(trianglesAt, array.triangleBlocks.data(), array.triangleBlocks.size() * 4ull);

        const auto at = [&](std::uint64_t poolAt) { return static_cast<std::uint32_t>(linked.poolOffset + poolAt); };
        bakes.push_back({at(recordsAt), blocks, at(statesAt), at(0), at(trianglesAt), triangles, at(indicesAt)});
        arrayBuilds.push_back({id, blas.generation, std::move(histogram), linked.poolOffset, linked.poolOffset + descsAt, micromapScratchUsed});
        micromapScratchUsed += arrayScratch;
    }

    if (blocks) micromapLinked = true;
    if (blocks)
        log("Opacity micromaps: structure " + std::to_string(id) + " linked, " + std::to_string(blocks) + " blocks for " + std::to_string(triangles)
            + " triangles of " + std::to_string(layouts.size()) + " meshes, " + std::to_string(poolBytes / 1024) + " KB " + (blas.micromaps ? "relinking" : "first") + ".");

    // The array is built this frame; the structure links it in the next (finishMicromaps), so the array
    // build never shares a frame with the structure it belongs to.
    if (blas.micromaps) releaseMicromaps(*blas.micromaps);
    if (blocks)
    {
        linked.triangleCount = triangles;
        linked.levels = array.histogram;
        if (blas.micromaps) ++micromapRelinks;
        ++micromapLinks;
        blas.micromaps = std::move(linked);
    }
    else blas.micromaps.reset();
    blas.pendingLink = true;
    return true;
}

bool Scene::finishMicromaps(BlasId id)
{
    auto& blas = structures[id];
    if (!blas.live || !blas.pendingLink) return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = inputsOf(blas, false);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    const auto blasScratch = aligned(prebuild.ScratchDataSizeInBytes, 256);

    // New room, never the structure's own: rebuilding a linked structure where the last frames' top
    // levels still point at it ends in a device hang (2026-10-31, 17 links in during a load).
    const bool inPlace = false;
    std::optional<std::uint64_t> offset = blasAllocator.allocate(prebuild.ResultDataMaxSizeInBytes,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    if (!offset || scratchUsed + blasScratch > tlasScratch + BuildScratchBytes)
    {
        if (offset && !inPlace) blasAllocator.release(*offset, prebuild.ResultDataMaxSizeInBytes);
        log(std::string("Opacity micromaps: structure ") + std::to_string(id) + " not linked, "
            + (!offset ? "the structures' pool has no room for the structure" : "the frame's scratch has no room") + ".");
        return false;
    }

    if (!inPlace) releases.push_back({signalValue, Pool::Structures, blas.offset, blas.size});
    blas.offset = *offset;
    if (!inPlace) blas.size = prebuild.ResultDataMaxSizeInBytes;
    blas.scratch = blasScratch;
    blas.updateScratch = aligned(prebuild.UpdateScratchDataSizeInBytes, 256);
    blas.compacted = false;
    ++blas.serial;
    blas.pendingLink = false;

    // No compacted size asked for: a structure with micromaps is never compacted, and one that lost them
    // has no readback entry this frame.
    builds.push_back({id, blas.generation, false, scratchUsed, NoPostbuild});
    blas.queued = false;
    scratchUsed += blas.scratch;
    blas.built = true;
    return true;
}

void Scene::releaseMicromaps(const LinkedMicromaps& linked)
{
    if (linked.poolBytes) releases.push_back({signalValue, Pool::Micromaps, linked.poolOffset, linked.poolBytes});
    if (linked.arraySize) releases.push_back({signalValue, Pool::Structures, linked.arrayOffset, linked.arraySize});
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::micromapAddress(std::uint64_t offset) const
{
    return micromaps->GetGPUVirtualAddress() + offset;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Scene::inputsOf(const Blas& blas, bool update) const
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    // A structure with micromaps does not compact: compacting one that links an OMM array ends in a
    // device hang on this runtime (2026-10-31, with ALLOW_COMPACTION set properly, unlike the first
    // time), so linked statics keep their room and their size.
    inputs.Flags = blas.use == Use::Static && !blas.micromaps
        ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION
        : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
            | (blas.use == Use::Static ? (blas.micromaps ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS
                                                     : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE)
                                     : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
    if (update) inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    inputs.NumDescs = static_cast<UINT>(blas.geometries.size());
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = blas.micromaps ? blas.micromaps->geometries.data() : blas.geometries.data();
    return inputs;
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::address(const Blas& blas) const
{
    return blasPool->GetGPUVirtualAddress() + blas.offset;
}

void Scene::writeRow(std::uint32_t row, const GIGeometry& geometry)
{
    rows[row] = geometry;
    dirtyRows.push_back(row);
}

// A descriptor of the scene's texture range: the texture's levels of detail, or a null view.
void Scene::writeTexture(std::uint32_t textureSlot, ID3D12Resource* texture, DXGI_FORMAT format, std::uint32_t mips)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = mips;
    auto at = textureDescriptors;
    at.ptr += std::size_t{textureSlot} * descriptorSize;
    device->CreateShaderResourceView(texture, &desc, at);
}

// Room in the slot's upload buffer, aligned for any copy; UploadBytes when there is none.
std::uint64_t Scene::reserveUpload(std::uint64_t bytes)
{
    const auto at = aligned(uploadUsed, 16);
    if (at + bytes > UploadBytes) return UploadBytes;

    uploadUsed = at + bytes;
    return at;
}
}
