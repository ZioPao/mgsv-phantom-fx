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
                if (sizes[i]) compactionsDue.push_back({built[i].id, built[i].generation, sizes[i]});

            const D3D12_RANGE none{};
            postbuildReadback[slot]->Unmap(0, &none);
        }

        built.clear();
    }

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
            if (finishMicromaps(id, staticBuilds))
            {
                ++staticBuilds;
                triangles += blas.triangles;
            }
        }

        std::uint32_t prepared{};
        for (BlasId id = 0; id < structures.size() && prepared < MicromapLinksPerFrame; ++id)
        {
            auto& blas = structures[id];
            if (!blas.live || !blas.micromapsDirty || blas.pendingLink || blas.use != Use::Static) continue;

            // PHANTOM_FX_OMM_ONCE links one structure a session, for telling one link from a batch of them.
            if (GetEnvironmentVariableA("PHANTOM_FX_OMM_ONCE", nullptr, 0) && micromapLinked) return;
            // PHANTOM_FX_OMM_NO_RELINK leaves a structure's first micromaps in place: tells a fault in
            // re-linking (release and rebuild) from one in the first link.
            if (blas.micromaps && GetEnvironmentVariableA("PHANTOM_FX_OMM_NO_RELINK", nullptr, 0))
            {
                blas.micromapsDirty = false;
                continue;
            }

            if (!prepareMicromaps(id)) continue;
            blas.micromapsDirty = false;
            ++prepared;
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

        // The readback's k-th compacted size is the slot's k-th static build's.
        builds.push_back({id, blas.generation, false, scratchUsed, staticBuilds++});
        builtInSlot[slot].push_back({id, blas.generation});
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
        // A structure with micromaps was built without ALLOW_COMPACTION: copying it as compacted is
        // undefined, and was the in-game device removal's cause (docs/opacity-micromaps.md).
        if (!blas.live || blas.generation != due.generation || blas.compacted || blas.micromaps || due.size >= blas.size) continue;

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

void Scene::recordBottomLevels(ID3D12GraphicsCommandList4& list)
{
    D3D12_RESOURCE_BARRIER readable[]{uavBarrier(), transition(geometryPool.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list.ResourceBarrier(static_cast<UINT>(std::size(readable)), readable);

    // The baked micromaps' arrays, which the bottom levels built after link to.
    if (!bakes.empty())
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

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs = inputsOf(blas, build.update);
        desc.DestAccelerationStructureData = address(blas);
        desc.SourceAccelerationStructureData = build.update ? address(blas) : 0;
        desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress() + build.scratch;
        if (blas.use == Use::Static && !blas.micromaps)
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
        list.CopyRaytracingAccelerationStructure(blasPool->GetGPUVirtualAddress() + compaction.to, blasPool->GetGPUVirtualAddress() + compaction.from,
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);

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

    // A static structure's cut-out meshes keep what their micromaps are baked from once their textures come.
    if (micromapsOn && use == Use::Static)
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

    const auto first = rowAllocator.allocate(geometries.size());
    if (!first) return std::nullopt;

    const auto offset = blasAllocator.allocate(prebuild.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    if (!offset)
    {
        rowAllocator.release(*first, geometries.size());
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
    releases.push_back({signalValue, Pool::Rows, blas.firstGeometry, blas.geometries.size()});
    if (blas.micromaps) releaseMicromaps(*blas.micromaps);

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

    builds.push_back({id, blas.generation, false, scratchUsed, 0});
    scratchUsed += blas.scratch;
    blas.built = true;
    return true;
}

bool Scene::refit(BlasId id)
{
    auto& blas = structures[id];
    if (!blas.built) return rebuild(id);
    if (scratchUsed + blas.updateScratch > tlasScratch + BuildScratchBytes) return false;

    builds.push_back({id, blas.generation, true, scratchUsed, 0});
    scratchUsed += blas.updateScratch;
    return true;
}

void Scene::setMaterial(BlasId id, std::uint32_t geometry, const scene_description::Material& material)
{
    auto& blas = structures[id];
    if (!blas.live || geometry >= blas.geometries.size()) return;

    auto row = rows[blas.firstGeometry + geometry];
    const auto before = row;
    row.flags = (row.flags & GI_GEOMETRY_CUT_OUT) | (material.flags & ~GI_GEOMETRY_CUT_OUT);
    row.texture = material.texture;
    row.alphaThreshold = material.alphaThreshold;
    row.specularTexture = material.specularTexture;
    row.normalTexture = material.normalTexture;
    writeRow(blas.firstGeometry + geometry, row);

    // Its micromaps are baked from its texture's alpha: a new texture, threshold or role bakes them again.
    const bool hasSource = geometry < blas.sources.size() && !blas.sources[geometry].uvs.empty();
    if (hasSource && (row.texture != before.texture || row.alphaThreshold != before.alphaThreshold || row.flags != before.flags))
        blas.micromapsDirty = true;
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
    // tells a fault in building and releasing them from one in reading them.
    if (GetEnvironmentVariableA("PHANTOM_FX_OMM_DISABLE_INSTANCES", nullptr, 0)) flags |= D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS;
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
    for (const auto& blas : structures)
        if (blas.live && blas.micromaps)
        {
            ++result.micromapStructures;
            result.micromapBlocks += blas.micromaps->blocks;
        }
    result.micromapPoolUsed = micromapAllocator.usedSize();
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
            if (array.histogram[level]) histogram.push_back({array.histogram[level], level, D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE});

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
        if (GetEnvironmentVariableA("PHANTOM_FX_OMM_NOLINK", nullptr, 0)) continue;

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
            descs[b].Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
            const auto& row = rows[blas.firstGeometry + linkedGeometries[array.blockMesh[b]]];
            records[b] = {block.uvs[0], block.uvs[1], block.uvs[2], block.level, row.texture, block.byteOffset, row.alphaThreshold, 0};
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
    if (blocks) blas.micromaps = std::move(linked);
    else blas.micromaps.reset();
    blas.pendingLink = true;
    return true;
}

bool Scene::finishMicromaps(BlasId id, std::uint32_t postbuild)
{
    auto& blas = structures[id];
    if (!blas.live || !blas.pendingLink) return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = inputsOf(blas, false);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    const auto blasScratch = aligned(prebuild.ScratchDataSizeInBytes, 256);
    const auto offset = blasAllocator.allocate(prebuild.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    if (!offset || scratchUsed + blasScratch > tlasScratch + BuildScratchBytes)
    {
        if (offset) blasAllocator.release(*offset, prebuild.ResultDataMaxSizeInBytes);
        log(std::string("Opacity micromaps: structure ") + std::to_string(id) + " not linked, "
            + (!offset ? "the structures' pool has no room for the structure" : "the frame's scratch has no room") + ".");
        return false;
    }

    releases.push_back({signalValue, Pool::Structures, blas.offset, blas.size});
    blas.offset = *offset;
    blas.size = prebuild.ResultDataMaxSizeInBytes;
    blas.scratch = blasScratch;
    blas.updateScratch = aligned(prebuild.UpdateScratchDataSizeInBytes, 256);
    blas.compacted = false;
    blas.pendingLink = false;

    // No compacted size asked for: a structure with micromaps is never compacted (below).
    builds.push_back({id, blas.generation, false, scratchUsed, postbuild});
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
    // A structure with micromaps is not compacted: a copy of it would keep pointing at its array, and the
    // copies are a path the driver has to get right for both.
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
