// The global illumination's Direct3D 12 side; gi_renderer.hpp describes the route.
#include "gi_renderer.hpp"

#include "gi_d3d12.hpp"
#include "gi_light.hpp"
#include "gi_shadow.hpp"
#include "log.hpp"
#include "range_allocator.hpp"
#include "shaders/gi_shared.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// The kernels, compiled by DXC at build time (CMakeLists.txt).
#include "BlendScreenProbes.h"
#include "BuildInstances.h"
#include "ClearBounds.h"
#include "ClassifyShadowTiles.h"
#include "ClearBuffer.h"
#include "ClearCounters.h"
#include "ClearOutput.h"
#include "ClearProbeMask.h"
#include "CompactScreenProbes.h"
#include "CopyProbes.h"
#include "CountScreenProbes.h"
#include "DebugScene.h"
#include "FilterGI.h"
#include "FilterProbeMask.h"
#include "FilterScreenProbes.h"
#include "FilterShadows.h"
#include "GenerateDispatch.h"
#include "InitCachedTileLRU.h"
#include "InterpolateScreenProbes.h"
#include "LearnAlbedo.h"
#include "PatchScreenProbes.h"
#include "PopulateCells.h"
#include "PopulateMultibounceCells.h"
#include "PopulateScreenProbes.h"
#include "ProjectScreenProbes.h"
#include "PurgeTiles.h"
#include "ReorderScreenProbes.h"
#include "ReprojectGI.h"
#include "ReprojectScreenProbes.h"
#include "ResolveCells.h"
#include "SampleScreenProbes.h"
#include "ScanAddOffsets.h"
#include "ScanBlockSums.h"
#include "ScanBlocks.h"
#include "Scatter.h"
#include "ScatterScreenProbes.h"
#include "SpawnScreenProbes.h"
#include "StoreSpawnCount.h"
#include "TraceSunShadows.h"
#include "UpdateMultibounceCells.h"
#include "UpdateTiles.h"

namespace phantom_fx::gi_renderer
{
namespace
{
using namespace phantom_fx::gi;
using namespace phantom_fx::gi_d3d12;

// ReShade's proxies return their native object for this interface (IID_UnwrappedObject).
constexpr GUID UnwrappedObject{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
constexpr std::uint32_t FramesInFlight = 3;
constexpr DWORD FenceTimeout = 10'000;

// Memory: the vertex pool in vertices of 16 bytes, the rest in bytes. The pool holds the static
// meshes (MGSV's scenes hold about 2.5 million triangles, three vertices each), then the frame's
// skinned meshes, as many as a frame can capture.
constexpr std::uint64_t StaticVertices = 16ull << 20;
constexpr std::uint64_t SkinnedVertices = TransferVertices;
constexpr std::uint64_t VertexPoolVertices = StaticVertices + SkinnedVertices;
constexpr std::uint64_t BlasPoolBytes = 512ull << 20;
constexpr std::uint64_t ScratchBytes = 64ull << 20;
constexpr std::uint64_t UploadBytes = 8ull << 20;
constexpr std::uint32_t MaxInstances = 32768;
// The entries the scene keeps: an instance each, and one for the frame's skinned meshes.
constexpr std::uint32_t MaxLiveEntries = MaxInstances - 1;
// An entry the game has not drawn for this many frames leaves the scene.
constexpr std::uint32_t RetentionFrames = 3600;
// Where no albedo is known yet: MGSV's materials are dark.
constexpr float DefaultAlbedo = 0.2f;
// Tangent of the sun's angular radius, 0.27 degrees.
constexpr float SunRadius = 0.0047f;
// The sun's ray-traced shadows: how much of the sun an alpha-tested mesh blocks beyond the game's
// shadow map (its triangles lack the holes its texture cuts), and how far the rays look for what
// blocks the sun.
constexpr float AlphaTestedBlocking = 0.5f, ShadowRayLength = 5000;
// Diagnostics: the most phases a frame's list times.
constexpr std::uint32_t MaxMarks = 32;
// The static meshes' structures a frame builds at most; the rest wait for the next frames, so a new
// area costs a few frames a little rather than one frame a lot.
constexpr std::uint32_t BuildsPerFrame = 128;
constexpr std::uint64_t BuildTrianglesPerFrame = 300'000;

// The kernels' DXIL in gi_d3d12::Kernel's order.
constexpr std::span<const unsigned char> Bytecode[]{GI_ClearBounds, GI_Scatter, GI_BuildInstances, GI_DebugScene, GI_ClearOutput,
    GI_ClearCounters, GI_GenerateDispatch, GI_ClearBuffer, GI_ScanBlocks, GI_ScanBlockSums, GI_ScanAddOffsets, GI_ClearProbeMask,
    GI_FilterProbeMask, GI_InitCachedTileLRU, GI_ReprojectScreenProbes, GI_CountScreenProbes, GI_ScatterScreenProbes, GI_SpawnScreenProbes,
    GI_CompactScreenProbes, GI_PatchScreenProbes, GI_StoreSpawnCount, GI_SampleScreenProbes, GI_LearnAlbedo, GI_PopulateScreenProbes,
    GI_PurgeTiles, GI_PopulateMultibounceCells, GI_PopulateCells, GI_UpdateTiles, GI_UpdateMultibounceCells, GI_ResolveCells,
    GI_BlendScreenProbes, GI_CopyProbes, GI_ReorderScreenProbes, GI_FilterScreenProbes, GI_ProjectScreenProbes, GI_InterpolateScreenProbes,
    GI_ReprojectGI, GI_FilterGI, GI_TraceSunShadows, GI_ClassifyShadowTiles, GI_FilterShadows};
static_assert(std::size(Bytecode) == static_cast<std::size_t>(Kernel::Count));

// One captured static mesh on the GPU side.
struct Entry
{
    bool live{};
    bool hasVertices{};
    std::uint64_t vertexOffset{}, vertexCount{};
    bool hasBlas{}, built{}, needsBuild{};
    std::uint64_t blasOffset{}, blasSize{};
    std::uint32_t lastDrawn{};
    bool alphaTested{};
};

// What a bottom-level structure of so many vertices needs: the driver's answer, kept, as asking
// it costs about 25 microseconds.
struct Sizes
{
    std::uint64_t result{}, scratch{};
};

// Memory an evicted or replaced entry gives back once the GPU is past the frame that used it last.
struct Release
{
    std::uint64_t fenceValue{};
    bool blas{};
    std::uint64_t offset{}, size{};
};

// Direct3D 12's view of a texture Direct3D 11 shares, opened again when Direct3D 11 replaces it.
// Holding the texture keeps its address from being reused by another.
struct Shared
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D12Resource> resource;
};

struct State
{
    ComPtr<ID3D12Device> wrapper;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12CommandAllocator>, FramesInFlight> allocators;
    std::array<std::uint64_t, FramesInFlight> submitted{};
    ComPtr<ID3D12GraphicsCommandList4> list;
    HANDLE event{};
    std::uint64_t frames{};

    ComPtr<ID3D12RootSignature> rootSignature;
    Pipelines pipelines;
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptorSize{};

    ComPtr<ID3D12Resource> vertexPool, blasPool, scratch, entryBounds, entryAlbedo, instanceDescs, tlas, counters, dispatchArguments;
    std::uint64_t tlasSize{}, tlasScratchSize{};
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> upload;
    std::array<std::uint8_t*, FramesInFlight> uploadData{};

    RangeAllocator vertexAllocator{StaticVertices}, blasAllocator{BlasPoolBytes};
    std::unordered_map<std::uint64_t, Sizes> sizes; // By vertex count
    // The frame's skinned meshes in one structure, built again each frame.
    ComPtr<ID3D12Resource> skinnedBlas, skinnedScratch;
    std::vector<Entry> entries = std::vector<Entry>(MaxEntries);
    std::vector<Release> releases;
    std::uint32_t liveEntries{}, instances{};
    std::uint64_t triangles{};
    // Diagnostics: the live entries, and their triangles, by when the game last drew them: this
    // frame, within a second, within ten seconds, before.
    std::array<std::uint32_t, 4> entriesByAge{};
    std::array<std::uint64_t, 4> trianglesByAge{};

    std::unique_ptr<gi_light::Light> light;
    std::unique_ptr<gi_shadow::Shadows> shadows;

    // Diagnostics: GPU timestamps between the named phases of each frame's list (Recorder::mark),
    // read when the frame's slot comes round again. The scene's and the light's times are averaged
    // for the overlay; each phase's are summed for the log, with the frame's builds and CPU time.
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    std::uint64_t timestampFrequency{};
    std::array<std::vector<const char*>, FramesInFlight> marks;
    double sceneMilliseconds{}, lightMilliseconds{}, shadowMilliseconds{};
    struct Tally
    {
        std::vector<std::pair<const char*, double>> phases; // Milliseconds, in the order they first ran
        std::uint32_t timedFrames{}, frames{}, builds{}, mostBuilds{}, waiting{};
        std::uint64_t builtTriangles{}, skinnedTriangles{};
        double cpuMilliseconds{}, mostCpuMilliseconds{};
        std::uint64_t since{};
    } tally;

    // Diagnostics: a copy of the output every ten seconds, read once the GPU is past it.
    ComPtr<ID3D12Resource> statsReadback, statsCounters;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT statsFootprint{};
    std::uint64_t statsFence{}, lastStats{};
    Shared depth, normal, albedo, transfer, output, shadow;

    ~State()
    {
        if (event) CloseHandle(event);
    }
};
std::unique_ptr<State> state;

ComPtr<ID3D12Resource> openShared(State& s, Shared& shared, ID3D11Texture2D* texture, const char* what)
{
    if (shared.texture.Get() == texture) return shared.resource;

    ComPtr<IDXGIResource1> resource;
    check(texture->QueryInterface(IID_PPV_ARGS(&resource)), what);
    HANDLE handle{};
    check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle), what);
    ComPtr<ID3D12Resource> opened;
    const auto result = s.device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle);
    check(result, what);
    shared.texture = texture;
    shared.resource = opened;

    return opened;
}

void createPipelines(State& s)
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = SrvCount;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = UavCount;

    D3D12_ROOT_PARAMETER parameters[5]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b0: the frame's constants
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // b1: the dispatch's
    parameters[1].Constants.ShaderRegister = 1;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t0, space1: the scene
    parameters[4].Descriptor.RegisterSpace = 1;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (UINT i = 0; i < 2; ++i)
    {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(std::size(parameters));
    desc.pParameters = parameters;
    desc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    desc.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, errors;
    const auto result = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error("Serializing the root signature failed: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : hex(static_cast<std::uint32_t>(result))));

    check(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.rootSignature)), "Creating the root signature");

    for (std::size_t kernel = 0; kernel < s.pipelines.size(); ++kernel)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = s.rootSignature.Get();
        pipeline.CS = {Bytecode[kernel].data(), Bytecode[kernel].size()};
        check(s.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&s.pipelines[kernel])), "Creating a kernel");
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = 16;
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    check(s.device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&s.dispatchSignature)), "Creating the dispatch signature");
}

void createResources(State& s)
{
    auto* device = s.device.Get();
    s.vertexPool = createBuffer(device, VertexPoolVertices * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the vertex pool");
    s.blasPool = createBuffer(device, BlasPoolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the acceleration structure pool");
    s.scratch = createStorage(device, ScratchBytes, "Creating the build scratch");
    s.entryBounds = createStorage(device, std::uint64_t{MaxEntries} * 6 * 4, "Creating the entry bounds");
    s.entryAlbedo = createStorage(device, std::uint64_t{MaxEntries} * 16, "Creating the entry albedo");
    s.instanceDescs = createStorage(device, std::uint64_t{MaxInstances} * sizeof(GIInstanceDesc), "Creating the instance list");
    s.counters = createStorage(device, GI_COUNTER_COUNT * 4, "Creating the counters");
    s.dispatchArguments = createStorage(device, GI_DISPATCH_SLOTS * 16, "Creating the dispatch arguments");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    inputs.NumDescs = MaxInstances;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    s.tlasSize = prebuild.ResultDataMaxSizeInBytes;
    s.tlasScratchSize = (prebuild.ScratchDataSizeInBytes + 255) / 256 * 256;
    if (s.tlasScratchSize > ScratchBytes / 2) throw std::runtime_error("The scene's acceleration structure needs too much scratch memory.");

    s.tlas = createBuffer(device, s.tlasSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the scene's acceleration structure");

    D3D12_RAYTRACING_GEOMETRY_DESC skinned{};
    skinned.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    skinned.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    skinned.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    skinned.Triangles.VertexCount = static_cast<UINT>(SkinnedVertices);
    skinned.Triangles.VertexBuffer.StrideInBytes = 16;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS skinnedInputs{};
    skinnedInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    skinnedInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    skinnedInputs.NumDescs = 1;
    skinnedInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    skinnedInputs.pGeometryDescs = &skinned;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO skinnedPrebuild{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&skinnedInputs, &skinnedPrebuild);
    s.skinnedBlas = createBuffer(device, skinnedPrebuild.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "Creating the skinned meshes' structure");
    s.skinnedScratch = createStorage(device, skinnedPrebuild.ScratchDataSizeInBytes, "Creating the skinned meshes' build scratch");

    for (std::uint32_t slot = 0; slot < FramesInFlight; ++slot)
    {
        s.upload[slot] = createBuffer(device, UploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            "Creating an upload buffer");
        void* data{};
        check(s.upload[slot]->Map(0, nullptr, &data), "Mapping an upload buffer");
        s.uploadData[slot] = static_cast<std::uint8_t*>(data);
    }

    D3D12_QUERY_HEAP_DESC queries{};
    queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queries.Count = MaxMarks * FramesInFlight;
    check(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&s.timestamps)), "Creating the timestamps");
    s.timestampReadback = createBuffer(device, std::uint64_t{queries.Count} * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "Creating the timestamp readback");
    check(s.queue->GetTimestampFrequency(&s.timestampFrequency), "Reading the timestamp frequency");

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = TableSize * FramesInFlight;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s.heap)), "Creating the descriptor heap");
    s.descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void waitFor(State& s, std::uint64_t value)
{
    if (!value || s.fence->GetCompletedValue() >= value) return;

    check(s.fence->SetEventOnCompletion(value, s.event), "Waiting for Direct3D 12");
    if (WaitForSingleObject(s.event, FenceTimeout) != WAIT_OBJECT_0) throw std::runtime_error("The global illumination's GPU work did not finish within 10 seconds.");
}

// Gives back the memory whose last user the GPU has finished.
void collectReleases(State& s)
{
    const auto completed = s.fence->GetCompletedValue();
    std::erase_if(s.releases, [&](const Release& release) {
        if (release.fenceValue > completed) return false;

        (release.blas ? s.blasAllocator : s.vertexAllocator).release(release.offset, release.size);
        return true;
    });
}

void evict(State& s, std::uint32_t index, std::uint64_t fenceValue, std::vector<std::uint32_t>& evicted)
{
    auto& entry = s.entries[index];
    if (!entry.live) return;

    if (entry.hasVertices) s.releases.push_back({fenceValue, false, entry.vertexOffset, entry.vertexCount});
    if (entry.hasBlas) s.releases.push_back({fenceValue, true, entry.blasOffset, entry.blasSize});
    entry = {};
    evicted.push_back(index);
}

// The live entries the frame did not draw, the one drawn longest ago first.
std::vector<std::uint32_t> leastRecentlyDrawn(const State& s, const Frame& frame)
{
    std::vector<std::uint32_t> candidates;
    for (std::uint32_t i = 0; i < MaxEntries; ++i)
        if (s.entries[i].live && s.entries[i].lastDrawn != frame.frameIndex) candidates.push_back(i);

    std::sort(candidates.begin(), candidates.end(), [&](auto a, auto b) { return s.entries[a].lastDrawn < s.entries[b].lastDrawn; });

    return candidates;
}

// When `wanted` does not fit: the first time a frame, evicts the entries the game drew longest ago
// until an eighth of the pool, and at least `wanted`, is on its way back. The memory returns once
// the GPU is past this frame, so the allocation fails for now, as do the frame's others: the search
// runs once a frame, not once an allocation.
std::optional<std::uint64_t> allocateEvicting(State& s, RangeAllocator& allocator, bool blas, std::uint64_t wanted, std::uint64_t alignment,
    const Frame& frame, std::vector<std::uint32_t>& evicted, bool& evictedThisFrame)
{
    if (auto offset = allocator.allocate(wanted, alignment)) return offset;
    if (evictedThisFrame) return std::nullopt;

    evictedThisFrame = true;
    const auto target = std::max(wanted + alignment, allocator.capacity() / 8);
    std::uint64_t freed{};
    for (const auto index : leastRecentlyDrawn(s, frame))
    {
        if (freed >= target) break;

        const auto& entry = s.entries[index];
        freed += blas ? (entry.hasBlas ? entry.blasSize : 0) : (entry.hasVertices ? entry.vertexCount : 0);
        evict(s, index, frame.signalValue, evicted);
    }

    return std::nullopt;
}

const Sizes& sizesFor(State& s, D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs)
{
    const auto vertices = inputs.pGeometryDescs->Triangles.VertexCount;
    if (const auto found = s.sizes.find(vertices); found != s.sizes.end()) return found->second;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    s.device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    return s.sizes[vertices] = {prebuild.ResultDataMaxSizeInBytes, (prebuild.ScratchDataSizeInBytes + 255) / 256 * 256};
}

struct Uploads
{
    std::uint8_t* data{};
    D3D12_GPU_VIRTUAL_ADDRESS address{};
    std::uint64_t used{};

    // Room for `count` elements of `stride` bytes, aligned for a constant buffer and a structured view.
    std::uint64_t reserve(std::uint64_t stride, std::uint64_t count)
    {
        used = (used + 255) / 256 * 256;
        used = (used + stride - 1) / stride * stride;
        const auto offset = used;
        used += stride * std::max<std::uint64_t>(count, 1);
        if (used > UploadBytes) throw std::runtime_error("The global illumination's upload buffer is full.");

        return offset;
    }
};

// Gauss-Jordan with partial pivoting; projection-views are invertible.
Matrix inverse(const Matrix& input)
{
    std::array<double, 16> matrix{}, result{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) matrix[i] = input[i];

    for (int column = 0; column < 4; ++column)
    {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::abs(matrix[row * 4 + column]) > std::abs(matrix[pivot * 4 + column])) pivot = row;

        for (int k = 0; k < 4; ++k)
        {
            std::swap(matrix[column * 4 + k], matrix[pivot * 4 + k]);
            std::swap(result[column * 4 + k], result[pivot * 4 + k]);
        }

        const double scale = matrix[column * 4 + column] != 0 ? 1 / matrix[column * 4 + column] : 0;
        for (int k = 0; k < 4; ++k)
        {
            matrix[column * 4 + k] *= scale;
            result[column * 4 + k] *= scale;
        }

        for (int row = 0; row < 4; ++row)
        {
            if (row == column) continue;

            const double factor = matrix[row * 4 + column];
            for (int k = 0; k < 4; ++k)
            {
                matrix[row * 4 + k] -= factor * matrix[column * 4 + k];
                result[row * 4 + k] -= factor * result[column * 4 + k];
            }
        }
    }

    Matrix single{};
    for (int i = 0; i < 16; ++i) single[i] = static_cast<float>(result[i]);

    return single;
}

void copyMatrix(float4x4& to, const Matrix& from)
{
    std::memcpy(to.m, from.data(), sizeof(to.m));
}

GIFrameConstants constantsOf(const Frame& frame, std::uint32_t scatterCount, std::uint32_t instanceCount)
{
    GIFrameConstants constants{};
    copyMatrix(constants.viewProjection, frame.viewProjection);
    copyMatrix(constants.viewProjectionInverse, inverse(frame.viewProjection));
    copyMatrix(constants.previousViewProjection, frame.previousViewProjection);
    copyMatrix(constants.previousViewProjectionInverse, inverse(frame.previousViewProjection));
    constants.eye = {frame.eye[0], frame.eye[1], frame.eye[2], 0};
    constants.previousEye = {frame.previousEye[0], frame.previousEye[1], frame.previousEye[2], 0};
    constants.sunDirection = {frame.sunDirection[0], frame.sunDirection[1], frame.sunDirection[2], SunRadius};
    constants.sunColor = {frame.sunColor[0], frame.sunColor[1], frame.sunColor[2], 0};
    for (int i = 0; i < 9; ++i) constants.skySH[i] = {frame.skyRadiance[i][0], frame.skyRadiance[i][1], frame.skyRadiance[i][2], 0};
    constants.dimensions = {frame.width, frame.height, frame.frameIndex, instanceCount};
    constants.nearFar = {frame.nearPlane, frame.farPlane, DefaultAlbedo, frame.outputScale};
    constants.counts = {scatterCount, scatterCount, frame.showScene ? GI_VIEW_SCENE : GI_VIEW_OFF, 0};
    constants.shadowRange = {frame.shadowRange[0], frame.shadowRange[1], frame.shadowRange[2], frame.shadowRange[3]};
    constants.shadowParams = {frame.sunTangent, AlphaTestedBlocking, ShadowRayLength, 0};

    return constants;
}

std::uint32_t groups(std::uint64_t threads, std::uint32_t size)
{
    return static_cast<std::uint32_t>((threads + size - 1) / size);
}
}

void start(ID3D11Device* device11, HANDLE sharedFence)
{
    if (state) return;

    auto s = std::make_unique<State>();
    ComPtr<IDXGIDevice> dxgi;
    check(device11->QueryInterface(IID_PPV_ARGS(&dxgi)), "Finding the DXGI device");
    ComPtr<IDXGIAdapter> adapter;
    check(dxgi->GetAdapter(adapter.GetAddressOf()), "Finding the GPU");
    ComPtr<ID3D12Device> created, native;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created)), "Creating the Direct3D 12 device");
    if (SUCCEEDED(created->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(native.GetAddressOf())))) s->wrapper = created;
    else native = created;

    check(native.As(&s->device), "Finding Direct3D 12 ray tracing");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    s->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options));
    if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) throw std::runtime_error("The GPU or its driver cannot trace rays from compute shaders (DXR 1.1).");

    const D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_COMPUTE};
    check(s->device->CreateCommandQueue(&queue, IID_PPV_ARGS(&s->queue)), "Creating the Direct3D 12 queue");
    for (auto& allocator : s->allocators)
        check(s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator)), "Creating a command allocator");

    check(s->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, s->allocators[0].Get(), nullptr, IID_PPV_ARGS(&s->list)),
        "Creating the command list");
    check(s->list->Close(), "Closing the command list");
    check(s->device->OpenSharedHandle(sharedFence, IID_PPV_ARGS(&s->fence)), "Opening the shared fence");
    s->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s->event) throw std::runtime_error("Creating the fence event failed.");

    createPipelines(*s);
    createResources(*s);

    state = std::move(s);
    log("Global illumination started on Direct3D 12.");
}

void stop() noexcept
{
    if (!state) return;

    try
    {
        waitFor(*state, *std::max_element(state->submitted.begin(), state->submitted.end()));
    }
    catch (const std::exception& error)
    {
        log(error.what());
    }

    state.reset();
    log("Global illumination stopped.");
}

bool started() noexcept
{
    return state != nullptr;
}

// Diagnostics: the output light's level, logged once its copy is back.
void logStats(State& s)
{
    if (!s.statsFence || s.fence->GetCompletedValue() < s.statsFence) return;

    s.statsFence = 0;
    void* data{};
    if (FAILED(s.statsReadback->Map(0, nullptr, &data))) return;

    const auto& footprint = s.statsFootprint.Footprint;
    std::vector<float> luminance;
    double sum[3]{};
    for (UINT y = 0; y < footprint.Height; y += 8)
    {
        const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(data) + std::uint64_t{y} * footprint.RowPitch);
        for (UINT x = 0; x < footprint.Width; x += 8)
        {
            float rgb[3]{};
            for (int c = 0; c < 3; ++c)
            {
                // Half to float.
                const std::uint32_t h = row[x * 4 + c];
                const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
                float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                    : exponent == 31 ? 65504.0f : std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
                rgb[c] = sign ? -value : value;
                sum[c] += rgb[c];
            }

            luminance.push_back(0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]);
        }
    }

    s.statsReadback->Unmap(0, nullptr);
    if (luminance.empty()) return;

    // On the game's render thread: one partial sort a share, not a whole sort.
    const auto at = [&](double share) {
        const auto nth = luminance.begin() + static_cast<std::ptrdiff_t>(share * static_cast<double>(luminance.size() - 1));
        std::nth_element(luminance.begin(), nth, luminance.end());
        return std::to_string(*nth);
    };
    const auto count = static_cast<double>(luminance.size());
    log("Global illumination's light (luminance): 1% " + at(0.01) + ", 10% " + at(0.1) + ", median " + at(0.5) + ", 90% " + at(0.9) + ", 99% "
        + at(0.99) + ", max " + at(1.0) + "; mean rgb " + std::to_string(sum[0] / count) + " " + std::to_string(sum[1] / count) + " "
        + std::to_string(sum[2] / count) + ".");

    void* mapped{};
    if (FAILED(s.statsCounters->Map(0, nullptr, &mapped))) return;

    std::uint32_t counters[GI_COUNTER_COUNT]{};
    std::memcpy(counters, mapped, sizeof(counters));
    s.statsCounters->Unmap(0, nullptr);
    const auto value = [&](std::uint32_t counter) { return std::to_string(counters[counter]); };
    log("Global illumination's rays: " + std::to_string(counters[GI_COUNTER_COUNT - 1] * 64) + " from " + value(GI_COUNTER_COUNT - 1) + " probes, "
        + value(GI_COUNTER_HITS) + " hits (" + value(GI_COUNTER_NEAR_HITS) + " within 25 cm, " + value(GI_COUNTER_FULL_BUCKETS)
        + " without room in the cache), " + value(GI_COUNTER_VISIBILITY1) + " second bounces, " + value(GI_COUNTER_LIGHTS) + " hits lit, "
        + value(GI_COUNTER_COPIES) + " copies of a surface skipped, " + value(GI_COUNTER_UPDATE_TILES) + " cache tiles updated.");
}

// The phases a frame slot timed, once the GPU is past them: into the averages and the tally.
void readTimes(State& s, std::uint32_t slot)
{
    auto& marks = s.marks[slot];
    if (marks.size() < 2 || !s.timestampFrequency) return;

    void* data{};
    const D3D12_RANGE range{std::size_t{slot} * MaxMarks * 8, std::size_t{slot + 1} * MaxMarks * 8};
    if (FAILED(s.timestampReadback->Map(0, &range, &data))) return;

    const auto* ticks = reinterpret_cast<const std::uint64_t*>(static_cast<const std::uint8_t*>(data) + range.Begin);
    const double toMilliseconds = 1000.0 / static_cast<double>(s.timestampFrequency);
    double scene{}, light{}, shadow{};
    for (std::size_t i = 0; i + 1 < marks.size(); ++i)
    {
        const double milliseconds = ticks[i + 1] >= ticks[i] ? static_cast<double>(ticks[i + 1] - ticks[i]) * toMilliseconds : 0.0;
        // The scene's phases come first, up to the top level; the shadows' are named so.
        (i < 3 ? scene : std::strncmp(marks[i], "shadow", 6) == 0 ? shadow : light) += milliseconds;
        auto& phases = s.tally.phases;
        const auto found = std::find_if(phases.begin(), phases.end(), [&](const auto& phase) { return std::strcmp(phase.first, marks[i]) == 0; });
        if (found != phases.end()) found->second += milliseconds;
        else phases.emplace_back(marks[i], milliseconds);
    }

    const D3D12_RANGE none{};
    s.timestampReadback->Unmap(0, &none);
    marks.clear();
    s.sceneMilliseconds += (scene - s.sceneMilliseconds) * 0.05;
    s.lightMilliseconds += (light - s.lightMilliseconds) * 0.05;
    s.shadowMilliseconds += (shadow - s.shadowMilliseconds) * 0.05;
    ++s.tally.timedFrames;
}

// Diagnostics: every ten seconds, what a frame cost on average.
void logTally(State& s)
{
    auto& t = s.tally;
    const auto now = GetTickCount64();
    if (!t.since) t.since = now;
    if (now - t.since < 10'000 || !t.frames) return;

    const auto milliseconds = [](double value) {
        auto text = std::to_string(value);
        return text.substr(0, text.find('.') + 4);
    };

    std::string line = "Global illumination's GPU time a frame (Direct3D 12):";
    double total{};
    for (const auto& [phase, sum] : t.phases)
    {
        const double perFrame = t.timedFrames ? sum / t.timedFrames : 0.0;
        line += std::string(" ") + phase + " " + milliseconds(perFrame) + ",";
        total += perFrame;
    }

    log(line + " total " + milliseconds(total) + " ms. Static meshes' structures a frame: " + std::to_string(t.builds / t.frames) + " built ("
        + std::to_string(t.builtTriangles / t.frames / 1000) + "k triangles), at most " + std::to_string(t.mostBuilds) + " in a frame, "
        + std::to_string(t.waiting / t.frames) + " waiting; skinned meshes: " + std::to_string(t.skinnedTriangles / t.frames / 1000)
        + "k triangles. CPU a frame: " + milliseconds(t.cpuMilliseconds / t.frames) + " ms, at most " + milliseconds(t.mostCpuMilliseconds) + " ms.");
    t = {};
    t.since = now;
}

Result render(const Frame& frame)
{
    auto& s = *state;
    LARGE_INTEGER started{}, frequency{};
    QueryPerformanceCounter(&started);
    QueryPerformanceFrequency(&frequency);

    Result result;
    const auto slot = static_cast<std::uint32_t>(s.frames++ % FramesInFlight);
    waitFor(s, s.submitted[slot]);
    collectReleases(s);
    logStats(s);
    readTimes(s, slot);
    logTally(s);

    // A new render size starts the light and the shadows over. Each has memory only once traced;
    // the shadows give theirs back when turned off.
    const auto waitForAll = [&] { waitFor(s, *std::max_element(s.submitted.begin(), s.submitted.end())); };
    if (frame.traceLight && (!s.light || s.light->width() != frame.width || s.light->height() != frame.height))
    {
        waitForAll();
        s.light.reset();
        s.light = std::make_unique<gi_light::Light>(s.device.Get(), frame.width, frame.height);
        log("Global illumination's light at " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + ".");
    }

    if (frame.traceShadows && (!s.shadows || s.shadows->width() != frame.width || s.shadows->height() != frame.height))
    {
        waitForAll();
        s.shadows.reset();
        s.shadows = std::make_unique<gi_shadow::Shadows>(s.device.Get(), frame.width, frame.height);
        log("Ray-traced shadows at " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + ".");
    }
    else if (!frame.traceShadows && s.shadows)
    {
        waitForAll();
        s.shadows.reset();
    }

    const bool traceLight = frame.traceLight && s.light;
    const bool traceShadows = frame.traceShadows && s.shadows && frame.textures.shadow;

    // Entries the frame drew stay; eviction takes others first.
    for (const auto index : frame.drawn)
        if (index < MaxEntries && s.entries[index].live) s.entries[index].lastDrawn = frame.frameIndex;

    // ---- The frame's captures into the pool ----
    Uploads uploads{s.uploadData[slot], s.upload[slot]->GetGPUVirtualAddress()};
    const auto scatterOffset = uploads.reserve(sizeof(GIScatterRecord), frame.captures.size());
    auto* scatter = reinterpret_cast<GIScatterRecord*>(uploads.data + scatterOffset);
    std::uint32_t scatterCount{}, scatterVertices{}, skinnedVertices{};
    bool evictedVertices{}, evictedStructures{};
    for (const auto& capture : frame.captures)
    {
        if (!capture.vertexCount) continue;

        // The skinned meshes follow one another after the static meshes' room.
        if (capture.dynamic)
        {
            if (skinnedVertices + capture.vertexCount > SkinnedVertices) continue;

            scatter[scatterCount++] = {DynamicEntry, capture.transferOffset, capture.vertexCount, static_cast<std::uint32_t>(StaticVertices + skinnedVertices), 0, {}};
            scatterVertices = std::max(scatterVertices, capture.transferOffset + capture.vertexCount);
            skinnedVertices += capture.vertexCount;
            continue;
        }

        if (capture.entry >= DynamicEntry) continue;

        auto& entry = s.entries[capture.entry];
        const bool fresh = !entry.live;
        if (fresh)
        {
            entry = {};
            entry.live = true;
        }

        entry.lastDrawn = frame.frameIndex;
        entry.alphaTested |= capture.alphaTested;
        if (!entry.hasVertices || entry.vertexCount != capture.vertexCount)
        {
            if (entry.hasVertices) s.releases.push_back({frame.signalValue, false, entry.vertexOffset, entry.vertexCount});
            entry.hasVertices = false;
            const auto offset = allocateEvicting(s, s.vertexAllocator, false, capture.vertexCount, 1, frame, result.evicted, evictedVertices);
            if (!offset)
            {
                evict(s, capture.entry, frame.signalValue, result.evicted);
                continue;
            }

            entry.vertexOffset = *offset;
            entry.vertexCount = capture.vertexCount;
            entry.hasVertices = true;
        }

        // Records adjoin in the transfer texture, in the order they came.
        scatter[scatterCount++] = {capture.entry, capture.transferOffset, capture.vertexCount, static_cast<std::uint32_t>(entry.vertexOffset),
            fresh ? GI_SCATTER_NEW : 0u, {}};
        scatterVertices = std::max(scatterVertices, capture.transferOffset + capture.vertexCount);
        entry.needsBuild = true;
    }

    for (const auto index : frame.alphaTested)
        if (index < DynamicEntry && s.entries[index].live) s.entries[index].alphaTested = true;

    check(s.allocators[slot]->Reset(), "Resetting a command allocator");
    check(s.list->Reset(s.allocators[slot].Get(), nullptr), "Resetting the command list");
    auto& list = *s.list.Get();
    try
    {
        auto* depth = openShared(s, s.depth, frame.textures.depth, "Opening the depth").Get();
        auto* normal = openShared(s, s.normal, frame.textures.normal, "Opening the normals").Get();
        auto* albedo = openShared(s, s.albedo, frame.textures.albedo, "Opening the albedo").Get();
        auto* transfer = openShared(s, s.transfer, frame.textures.transfer, "Opening the transfer texture").Get();
        auto* output = openShared(s, s.output, frame.textures.output, "Opening the output").Get();
        auto* shadow = traceShadows ? openShared(s, s.shadow, frame.textures.shadow, "Opening the shadow").Get() : nullptr;

        // ---- Bottom-level acceleration structures for the entries whose geometry changed ----
        std::uint64_t scratchUsed = s.tlasScratchSize, budgetTriangles{};
        struct Build
        {
            std::uint32_t entry;
            D3D12_RAYTRACING_GEOMETRY_DESC geometry;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc;
        };
        std::vector<Build> builds;
        for (std::uint32_t index = 0; index < MaxEntries; ++index)
        {
            auto& entry = s.entries[index];
            if (!entry.live || !entry.needsBuild || !entry.hasVertices) continue;

            // The rest wait for the next frames.
            if (builds.size() >= BuildsPerFrame || budgetTriangles >= BuildTrianglesPerFrame)
            {
                ++s.tally.waiting;
                continue;
            }

            Build build{index};
            build.geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            build.geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            build.geometry.Triangles.VertexBuffer = {s.vertexPool->GetGPUVirtualAddress() + entry.vertexOffset * 16, 16};
            build.geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            build.geometry.Triangles.VertexCount = static_cast<UINT>(entry.vertexCount);
            auto& inputs = build.desc.Inputs;
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.NumDescs = 1;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.pGeometryDescs = &build.geometry;
            const auto sizes = sizesFor(s, inputs);
            // The rest waits for the next frame's scratch.
            if (scratchUsed + sizes.scratch > ScratchBytes) break;

            if (!entry.hasBlas || entry.blasSize < sizes.result)
            {
                if (entry.hasBlas) s.releases.push_back({frame.signalValue, true, entry.blasOffset, entry.blasSize});
                entry.hasBlas = entry.built = false;
                const auto offset = allocateEvicting(s, s.blasAllocator, true, sizes.result, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
                    frame, result.evicted, evictedStructures);
                if (!offset)
                {
                    evict(s, index, frame.signalValue, result.evicted);
                    continue;
                }

                entry.blasOffset = *offset;
                entry.blasSize = sizes.result;
                entry.hasBlas = true;
            }

            build.desc.DestAccelerationStructureData = s.blasPool->GetGPUVirtualAddress() + entry.blasOffset;
            build.desc.ScratchAccelerationStructureData = s.scratch->GetGPUVirtualAddress() + scratchUsed;
            scratchUsed += sizes.scratch;
            budgetTriangles += entry.vertexCount / 3;
            builds.push_back(build);
            // Recorded below, before anything traces.
            entry.built = true;
            entry.needsBuild = false;
        }

        // ---- The frame's instances ----
        // Past MaxLiveEntries, the entries drawn longest ago leave, an eighth of the instances more
        // than needed so the sort is rare. Left to the instance limit, whichever entries came last
        // were cut, the skinned meshes always: whole rooms and the player went missing.
        const auto liveEntries = static_cast<std::uint32_t>(std::count_if(s.entries.begin(), s.entries.end(), [](const Entry& entry) { return entry.live; }));
        if (liveEntries > MaxLiveEntries)
        {
            auto excess = liveEntries - MaxLiveEntries + MaxInstances / 8;
            for (const auto index : leastRecentlyDrawn(s, frame))
            {
                if (!excess--) break;

                evict(s, index, frame.signalValue, result.evicted);
            }
        }

        const auto instanceOffset = uploads.reserve(sizeof(GIInstanceRecord), MaxInstances);
        auto* instances = reinterpret_cast<GIInstanceRecord*>(uploads.data + instanceOffset);
        std::uint32_t instanceCount{};
        std::uint64_t triangles{};
        std::uint32_t live{};
        std::array<std::uint32_t, 4> entriesByAge{};
        std::array<std::uint64_t, 4> trianglesByAge{};
        if (skinnedVertices)
        {
            const auto blas = s.skinnedBlas->GetGPUVirtualAddress();
            instances[instanceCount++] = {DynamicEntry, static_cast<std::uint32_t>(StaticVertices), skinnedVertices / 3, GI_INSTANCE_DRAWN | GI_INSTANCE_DYNAMIC,
                {static_cast<std::uint32_t>(blas), static_cast<std::uint32_t>(blas >> 32)}, {}};
            triangles += skinnedVertices / 3;
        }

        for (std::uint32_t index = 0; index < MaxEntries; ++index)
        {
            auto& entry = s.entries[index];
            if (!entry.live) continue;

            const bool drawn = entry.lastDrawn == frame.frameIndex;
            if (frame.frameIndex - entry.lastDrawn > RetentionFrames)
            {
                evict(s, index, frame.signalValue, result.evicted);
                continue;
            }

            ++live;
            const auto age = frame.frameIndex - entry.lastDrawn;
            const auto bucket = age == 0 ? 0 : age <= 60 ? 1 : age <= 600 ? 2 : 3;
            ++entriesByAge[bucket];
            trianglesByAge[bucket] += entry.vertexCount / 3;
            if (!entry.built || instanceCount >= MaxInstances) continue;

            const auto blas = s.blasPool->GetGPUVirtualAddress() + entry.blasOffset;
            const auto flags = (drawn ? GI_INSTANCE_DRAWN : 0u) | (entry.alphaTested ? GI_INSTANCE_ALPHA_TESTED : 0u);
            instances[instanceCount++] = {index, static_cast<std::uint32_t>(entry.vertexOffset), static_cast<std::uint32_t>(entry.vertexCount / 3),
                flags, {static_cast<std::uint32_t>(blas), static_cast<std::uint32_t>(blas >> 32)}, {}};
            triangles += entry.vertexCount / 3;
        }

        s.liveEntries = live;
        s.instances = instanceCount;
        s.triangles = triangles;
        s.entriesByAge = entriesByAge;
        s.trianglesByAge = trianglesByAge;

        const auto parity = frame.frameIndex & 1;
        const auto constantsOffset = uploads.reserve(256, (sizeof(GIFrameConstants) + 255) / 256);
        auto constants = constantsOf(frame, scatterCount, instanceCount);
        if (traceLight) s.light->fill(constants, frame.verticalFov, parity);
        if (traceShadows) s.shadows->fill(constants);
        std::memcpy(uploads.data + constantsOffset, &constants, sizeof(constants));

        // ---- Descriptors ----
        Descriptors descriptors(s.device.Get(), s.heap.Get(), s.descriptorSize, slot);
        descriptors.texture(0, depth, DXGI_FORMAT_R32_FLOAT);
        descriptors.texture(1, normal, DXGI_FORMAT_R10G10B10A2_UNORM);
        descriptors.texture(2, albedo, DXGI_FORMAT_R8G8B8A8_UNORM);
        descriptors.texture(3, transfer, DXGI_FORMAT_R32G32B32A32_FLOAT);
        descriptors.buffer(4, s.upload[slot].Get(), scatterOffset, sizeof(GIScatterRecord), scatterCount);
        descriptors.buffer(5, s.upload[slot].Get(), instanceOffset, sizeof(GIInstanceRecord), instanceCount);
        descriptors.buffer(6, s.vertexPool.Get(), 0, 16, VertexPoolVertices);
        descriptors.outputTexture(0, output, DXGI_FORMAT_R16G16B16A16_FLOAT);
        descriptors.outputBuffer(1, s.vertexPool.Get(), 16, VertexPoolVertices);
        descriptors.outputBuffer(2, s.entryBounds.Get(), 4, std::uint64_t{MaxEntries} * 6);
        descriptors.outputBuffer(3, s.instanceDescs.Get(), sizeof(GIInstanceDesc), MaxInstances);
        descriptors.outputBuffer(4, s.counters.Get(), 4, GI_COUNTER_COUNT);
        descriptors.outputBuffer(5, s.dispatchArguments.Get(), 16, GI_DISPATCH_SLOTS);
        descriptors.outputBuffer(6, s.entryAlbedo.Get(), 16, MaxEntries);
        if (traceLight) s.light->describe(descriptors, parity);
        if (traceShadows) s.shadows->describe(descriptors, parity, shadow);

        ID3D12DescriptorHeap* heaps[]{s.heap.Get()};
        list.SetDescriptorHeaps(1, heaps);
        list.SetComputeRootSignature(s.rootSignature.Get());
        list.SetComputeRootConstantBufferView(0, uploads.address + constantsOffset);
        list.SetComputeRootDescriptorTable(2, descriptors.srvTable());
        list.SetComputeRootDescriptorTable(3, descriptors.uavTable());
        list.SetComputeRootShaderResourceView(4, s.tlas->GetGPUVirtualAddress());
        Recorder recorder(list, s.pipelines, s.dispatchSignature.Get(), s.dispatchArguments.Get());
        auto& marks = s.marks[slot];
        marks.clear();
        recorder.marker = [&](const char* phase) {
            if (marks.size() >= MaxMarks) return;

            list.EndQuery(s.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * MaxMarks + static_cast<UINT>(marks.size()));
            marks.push_back(phase ? phase : "");
        };

        recorder.mark("scatter");
        D3D12_RESOURCE_BARRIER begin[]{
            transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(normal, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(albedo, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(transfer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition(s.vertexPool.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        list.ResourceBarrier(static_cast<UINT>(std::size(begin)), begin);
        if (traceShadows) recorder.transition(shadow, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (scatterCount)
        {
            recorder.dispatch(Kernel::ClearBounds, groups(scatterCount, 64));
            recorder.barrier();
            recorder.dispatch(Kernel::Scatter, groups(scatterVertices, 64), 1, {scatterVertices});
        }

        D3D12_RESOURCE_BARRIER scattered[]{
            uavBarrier(),
            transition(s.vertexPool.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        list.ResourceBarrier(static_cast<UINT>(std::size(scattered)), scattered);

        recorder.mark("bottom-level structures");
        std::uint32_t builtStructures{};
        for (auto& build : builds)
        {
            // An entry evicted since its build was planned keeps nothing the build writes.
            if (!s.entries[build.entry].live) continue;

            build.desc.Inputs.pGeometryDescs = &build.geometry;
            list.BuildRaytracingAccelerationStructure(&build.desc, 0, nullptr);
            ++builtStructures;
            s.tally.builtTriangles += build.geometry.Triangles.VertexCount / 3;
        }

        s.tally.builds += builtStructures;
        s.tally.mostBuilds = std::max(s.tally.mostBuilds, builtStructures);
        if (skinnedVertices)
        {
            D3D12_RAYTRACING_GEOMETRY_DESC skinned{};
            skinned.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            skinned.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            skinned.Triangles.VertexBuffer = {s.vertexPool->GetGPUVirtualAddress() + StaticVertices * 16, 16};
            skinned.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            skinned.Triangles.VertexCount = skinnedVertices;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
            desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            desc.Inputs.NumDescs = 1;
            desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            desc.Inputs.pGeometryDescs = &skinned;
            desc.DestAccelerationStructureData = s.skinnedBlas->GetGPUVirtualAddress();
            desc.ScratchAccelerationStructureData = s.skinnedScratch->GetGPUVirtualAddress();
            list.BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
            s.tally.skinnedTriangles += skinnedVertices / 3;
        }

        // The instance list reads the bounds; the scene's build reads the bottom levels and the list.
        recorder.mark("instances and top level");
        recorder.dispatch(Kernel::BuildInstances, groups(instanceCount, 64));
        D3D12_RESOURCE_BARRIER built[]{
            uavBarrier(),
            transition(s.instanceDescs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        list.ResourceBarrier(static_cast<UINT>(std::size(built)), built);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC top{};
        top.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        top.Inputs.NumDescs = instanceCount;
        top.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        top.Inputs.InstanceDescs = s.instanceDescs->GetGPUVirtualAddress();
        top.DestAccelerationStructureData = s.tlas->GetGPUVirtualAddress();
        top.ScratchAccelerationStructureData = s.scratch->GetGPUVirtualAddress();
        list.BuildRaytracingAccelerationStructure(&top, 0, nullptr);

        D3D12_RESOURCE_BARRIER scene[]{
            uavBarrier(),
            transition(s.instanceDescs.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        list.ResourceBarrier(static_cast<UINT>(std::size(scene)), scene);

        // ---- The shadows, then the light or the debug view ----
        if (traceShadows) s.shadows->record(recorder);
        if (frame.showScene)
        {
            recorder.mark("scene view");
            recorder.dispatch(Kernel::DebugScene, groups(frame.width, 8), groups(frame.height, 8));
        }
        else if (traceLight) s.light->record(recorder, parity, depth, normal);

        // Diagnostics: every ten seconds, a copy of the output to read back.
        const auto now = GetTickCount64();
        if (!s.statsFence && now - s.lastStats > 10'000 && !frame.showScene && traceLight)
        {
            s.lastStats = now;
            const auto desc = output->GetDesc();
            UINT64 bytes{};
            s.device->GetCopyableFootprints(&desc, 0, 1, 0, &s.statsFootprint, nullptr, nullptr, &bytes);
            if (!s.statsReadback || s.statsReadback->GetDesc().Width < bytes)
                s.statsReadback = createBuffer(s.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_FLAG_NONE, "Creating the light's readback");
            if (!s.statsCounters)
                s.statsCounters = createBuffer(s.device.Get(), GI_COUNTER_COUNT * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_FLAG_NONE, "Creating the counters' readback");

            recorder.transition(s.counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list.CopyBufferRegion(s.statsCounters.Get(), 0, s.counters.Get(), 0, GI_COUNTER_COUNT * 4);
            recorder.transition(s.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            recorder.transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            from.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION to{s.statsReadback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            to.PlacedFootprint = s.statsFootprint;
            list.CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            recorder.transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.statsFence = frame.signalValue;
        }

        D3D12_RESOURCE_BARRIER end[]{
            uavBarrier(),
            transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(albedo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(transfer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
        };
        list.ResourceBarrier(static_cast<UINT>(std::size(end)), end);
        if (traceShadows) recorder.transition(shadow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        recorder.mark(nullptr);
        list.ResolveQueryData(s.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * MaxMarks, static_cast<UINT>(marks.size()), s.timestampReadback.Get(),
            std::uint64_t{slot} * MaxMarks * 8);
        check(list.Close(), "Recording the global illumination");
    }
    catch (...)
    {
        // The next frame resets the list, which it can only once closed.
        list.Close();
        throw;
    }

    check(s.queue->Wait(s.fence.Get(), frame.waitValue), "Waiting for the global illumination's inputs");
    ID3D12CommandList* lists[]{s.list.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    check(s.queue->Signal(s.fence.Get(), frame.signalValue), "Signalling the global illumination");
    s.submitted[slot] = frame.signalValue;

    LARGE_INTEGER finished{};
    QueryPerformanceCounter(&finished);
    const double cpu = static_cast<double>(finished.QuadPart - started.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    ++s.tally.frames;
    s.tally.cpuMilliseconds += cpu;
    s.tally.mostCpuMilliseconds = std::max(s.tally.mostCpuMilliseconds, cpu);

    return result;
}

std::string status()
{
    if (!state) return {};

    const auto& s = *state;
    const auto percent = [](const RangeAllocator& allocator) {
        return std::to_string(allocator.capacity() ? allocator.usedSize() * 100 / allocator.capacity() : 0) + "%";
    };

    const auto milliseconds = [](double value) {
        auto text = std::to_string(value);
        return text.substr(0, text.find('.') + 3) + " ms";
    };

    const auto aged = [&](int bucket) {
        return std::to_string(s.entriesByAge[bucket]) + " (" + std::to_string(s.trianglesByAge[bucket] / 1000) + "k triangles)";
    };

    return std::to_string(s.instances) + " instances of " + std::to_string(s.liveEntries) + " entries, " + std::to_string(s.triangles / 1000)
        + "k triangles; vertex pool " + percent(s.vertexAllocator) + ", acceleration structures " + percent(s.blasAllocator) + "; GPU: scene "
        + milliseconds(s.sceneMilliseconds) + ", light " + milliseconds(s.lightMilliseconds) + ", shadows " + milliseconds(s.shadowMilliseconds)
        + "; entries drawn this frame " + aged(0)
        + ", within a second " + aged(1) + ", within ten seconds " + aged(2) + ", before " + aged(3);
}
}
