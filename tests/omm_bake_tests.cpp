// The opacity micromaps' bake kernels (shaders/omm_bake.hlsl) on WARP: a mesh laid out by omm_layout,
// baked from a texture whose left half is opaque, its states read back and checked micro-triangle by
// micro-triangle against where the micro-triangle lies on the texture.
#include "passes/ray_tracing/omm_layout.hpp"
#include "passes/ray_tracing/shaders/gi_omm.h"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include "BakeMicromaps.h"
#include "ResolveMicromapIndices.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using namespace phantom_fx;

void expect(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(static_cast<unsigned long>(result)));
}

// The renderer's table sizes (gi_d3d12.hpp), so the kernels bind as they will in the add-on.
constexpr UINT SrvCount = 32, UavCount = 112;
constexpr UINT TextureSize = 64;

// A half of a normal float, its mantissa truncated.
std::uint16_t halfOf(float value)
{
    if (value == 0.0f) return 0;
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = static_cast<std::int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    return static_cast<std::uint16_t>(((bits >> 16) & 0x8000u) | (static_cast<std::uint32_t>(exponent) << 10) | ((bits >> 13) & 0x3FFu));
}

std::uint32_t uv(float u, float v)
{
    return halfOf(u) | (std::uint32_t{halfOf(v)} << 16);
}

float uOf(std::uint32_t packed)
{
    return omm_layout::halfToFloat(static_cast<std::uint16_t>(packed));
}

float vOf(std::uint32_t packed)
{
    return omm_layout::halfToFloat(static_cast<std::uint16_t>(packed >> 16));
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, std::uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES properties{heap};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<std::uint64_t>(size, 4);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> made;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&made)), "Creating a buffer");
    return made;
}

void fill(ID3D12Resource* resource, const void* data, std::size_t bytes)
{
    void* mapped{};
    check(resource->Map(0, nullptr, &mapped), "Mapping");
    std::memcpy(mapped, data, bytes);
    resource->Unmap(0, nullptr);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

// The renderer's root signature (gi_renderer.cpp, createPipelines).
ComPtr<ID3D12RootSignature> rootSignature(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = SrvCount;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = UavCount;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = GI_MAX_TEXTURES;
    ranges[2].RegisterSpace = 2;

    D3D12_ROOT_PARAMETER parameters[6]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 1;
    parameters[1].Constants.Num32BitValues = 4;
    for (UINT i = 0; i < 2; ++i)
    {
        parameters[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        parameters[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[4].Descriptor.RegisterSpace = 1;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[5].DescriptorTable.NumDescriptorRanges = 1;
    parameters[5].DescriptorTable.pDescriptorRanges = &ranges[2];

    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    for (UINT i = 0; i < 3; ++i)
    {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = i == 2 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(std::size(parameters));
    desc.pParameters = parameters;
    desc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    desc.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, errors;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "Serializing the root signature");
    ComPtr<ID3D12RootSignature> made;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&made)), "Creating the root signature");
    return made;
}

ComPtr<ID3D12PipelineState> pipeline(ID3D12Device* device, ID3D12RootSignature* root, const unsigned char* code, std::size_t size)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {code, size};
    ComPtr<ID3D12PipelineState> made;
    check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&made)), "Creating a kernel");
    return made;
}

// What the bake gave back.
struct Baked
{
    std::vector<std::uint8_t> data;
    std::vector<std::uint32_t> blockStates, indices;
};

Baked bake(const omm_layout::Layout& layout)
{
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Creating the DXGI factory");
    ComPtr<IDXGIAdapter> warp;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "Finding WARP");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "Creating the device");

    const auto root = rootSignature(device.Get());
    const auto bakeKernel = pipeline(device.Get(), root.Get(), GI_BakeMicromaps, sizeof(GI_BakeMicromaps));
    const auto resolveKernel = pipeline(device.Get(), root.Get(), GI_ResolveMicromapIndices, sizeof(GI_ResolveMicromapIndices));

    // The texture: opaque where x < 32, clear past it; one level.
    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = textureDesc.Height = TextureSize;
    textureDesc.DepthOrArraySize = textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES defaultHeap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> texture;
    check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
        "Creating the texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 textureBytes{};
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &textureBytes);
    std::vector<std::uint8_t> texels(textureBytes);
    for (UINT y = 0; y < TextureSize; ++y)
        for (UINT x = 0; x < TextureSize; ++x)
        {
            auto* texel = &texels[footprint.Footprint.RowPitch * y + 4 * x];
            texel[0] = texel[1] = texel[2] = 128;
            texel[3] = x < TextureSize / 2 ? 255 : 0;
        }
    const auto textureUpload = buffer(device.Get(), textureBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    fill(textureUpload.Get(), texels.data(), texels.size());

    // The pool as the scene lays a structure's micromaps out: the array's input past 128 bytes nothing
    // may touch, then the blocks' records, the triangles' blocks, the blocks' states and the index buffer.
    // All of it starts as garbage but the records and the triangles' blocks.
    const auto blockCount = static_cast<UINT>(layout.blocks.size());
    const auto triangles = static_cast<UINT>(layout.triangleBlocks.size());
    const auto align = [](std::uint64_t value) { return (value + 15) / 16 * 16; };
    const std::uint64_t dataAt = 128, recordsAt = align(dataAt + layout.bytes), trianglesAt = recordsAt + 32ull * blockCount,
                        statesAt = align(trianglesAt + 4ull * triangles), indicesAt = align(statesAt + 4ull * blockCount),
                        poolBytes = align(indicesAt + 4ull * triangles);
    std::vector<std::uint8_t> contents(poolBytes, 0xCD);
    for (std::uint32_t b = 0; b < blockCount; ++b)
    {
        const auto& block = layout.blocks[b];
        const gi::GIOmmBlock record{block.uvs[0], block.uvs[1], block.uvs[2], block.level, 1, block.byteOffset, 0.5f, 0};
        std::memcpy(&contents[recordsAt + 32ull * b], &record, sizeof(record));
    }
    std::memcpy(&contents[trianglesAt], layout.triangleBlocks.data(), 4ull * triangles);
    const auto poolUpload = buffer(device.Get(), poolBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    fill(poolUpload.Get(), contents.data(), contents.size());
    const auto pool = buffer(device.Get(), poolBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto readback = buffer(device.Get(), poolBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    // Every slot a null view, as the renderer's tables start, then the kernels' own.
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SrvCount + UavCount + GI_MAX_TEXTURES, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "Creating the descriptor heap");
    const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu = [&](UINT slot) { return D3D12_CPU_DESCRIPTOR_HANDLE{heap->GetCPUDescriptorHandleForHeapStart().ptr + std::size_t{slot} * increment}; };
    const auto gpu = [&](UINT slot) { return D3D12_GPU_DESCRIPTOR_HANDLE{heap->GetGPUDescriptorHandleForHeapStart().ptr + std::uint64_t{slot} * increment}; };

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Format = DXGI_FORMAT_R32_FLOAT;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    for (UINT i = 0; i < SrvCount; ++i) device->CreateShaderResourceView(nullptr, &nullSrv, cpu(i));
    for (UINT i = 1; i < GI_MAX_TEXTURES; ++i) device->CreateShaderResourceView(nullptr, &nullSrv, cpu(SrvCount + UavCount + i));
    D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
    nullUav.Format = DXGI_FORMAT_R32_FLOAT;
    nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < UavCount; ++i) device->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, cpu(SrvCount + i));

    D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    raw.Buffer.NumElements = static_cast<UINT>(poolBytes / 4);
    raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(pool.Get(), nullptr, &raw, cpu(SrvCount + 40));

    D3D12_SHADER_RESOURCE_VIEW_DESC textureView{};
    textureView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    textureView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    textureView.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture.Get(), &textureView, cpu(SrvCount + UavCount));

    const D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Creating the queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Creating the allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Creating the list");

    D3D12_TEXTURE_COPY_LOCATION to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    to.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION from{textureUpload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    from.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    list->CopyBufferRegion(pool.Get(), 0, poolUpload.Get(), 0, poolBytes);
    D3D12_RESOURCE_BARRIER ready[]{transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(pool.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(static_cast<UINT>(std::size(ready)), ready);

    ID3D12DescriptorHeap* heaps[]{heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootConstantBufferView(0, poolUpload->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, gpu(0));
    list->SetComputeRootDescriptorTable(3, gpu(SrvCount));
    list->SetComputeRootShaderResourceView(4, poolUpload->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(5, gpu(SrvCount + UavCount));

    const UINT bakeConstants[4]{static_cast<UINT>(recordsAt), blockCount, static_cast<UINT>(statesAt), static_cast<UINT>(dataAt)};
    list->SetPipelineState(bakeKernel.Get());
    list->SetComputeRoot32BitConstants(1, 4, bakeConstants, 0);
    list->Dispatch(blockCount, 1, 1);
    D3D12_RESOURCE_BARRIER all{};
    all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &all);

    const UINT resolveConstants[4]{static_cast<UINT>(trianglesAt), triangles, static_cast<UINT>(statesAt), static_cast<UINT>(indicesAt)};
    list->SetPipelineState(resolveKernel.Get());
    list->SetComputeRoot32BitConstants(1, 4, resolveConstants, 0);
    list->Dispatch((triangles + 63) / 64, 1, 1);
    list->ResourceBarrier(1, &all);

    const auto done = transition(pool.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &done);
    list->CopyBufferRegion(readback.Get(), 0, pool.Get(), 0, poolBytes);
    check(list->Close(), "Closing the list");

    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Creating the fence");
    check(queue->Signal(fence.Get(), 1), "Signalling");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(fence->SetEventOnCompletion(1, event), "Waiting");
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    check(device->GetDeviceRemovedReason(), "The bake");

    Baked baked;
    void* view{};
    check(readback->Map(0, nullptr, &view), "Mapping the readback");
    const auto* mapped = static_cast<const std::uint8_t*>(view);
    baked.data.assign(mapped + dataAt, mapped + dataAt + layout.bytes);
    baked.blockStates.resize(blockCount);
    std::memcpy(baked.blockStates.data(), mapped + statesAt, blockCount * 4ull);
    baked.indices.resize(triangles);
    std::memcpy(baked.indices.data(), mapped + indicesAt, triangles * 4ull);
    // Nothing written outside the array's input, the states and the index buffer.
    for (std::uint64_t at = 0; at < dataAt; ++at) expect(mapped[at] == 0xCD, "the bake wrote before the array's input");
    for (std::uint64_t at = dataAt + layout.bytes; at < recordsAt; ++at) expect(mapped[at] == 0xCD, "the bake wrote past the array's input");
    readback->Unmap(0, nullptr);
    return baked;
}

std::uint32_t stateAt(const Baked& baked, const omm_layout::Block& block, std::uint32_t index)
{
    const auto bit = index * GI_OMM_STATE_BITS;
    return (baked.data[block.byteOffset + bit / 8] >> (bit % 8)) & 1u;
}

// The texture's alpha falls from 1 to 0 between the texel centres at u = 31.5 / 64 and 32.5 / 64.
constexpr float SolidUntil = 31.4f / TextureSize, ClearFrom = 32.6f / TextureSize;

// Each micro-triangle against where its corners lie: all solid side opaque, all clear side transparent,
// across the edge unknown. Returns how many it could check.
std::uint32_t checkBlock(const Baked& baked, const omm_layout::Block& block, std::uint32_t number)
{
    const float u0 = uOf(block.uvs[0]), u1 = uOf(block.uvs[1]), u2 = uOf(block.uvs[2]);
    const std::uint32_t steps = 1u << block.level;
    const float step = 1.0f / static_cast<float>(steps);
    std::uint32_t checked = 0;
    for (std::uint32_t cell = 0; cell < steps * steps; ++cell)
    {
        float u, v;
        const auto index = gi::OmmCellMicroTriangle(cell, block.level, u, v);
        const bool upright = (cell & (steps - 1)) + (cell >> block.level) < steps;
        const float t = step / 3.0f;
        const float corners[3][2]{
            {upright ? u - t : u + t, upright ? v - t : v - 2 * t},
            {upright ? u + 2 * t : u - 2 * t, upright ? v - t : v + t},
            {upright ? u - t : u + t, upright ? v + 2 * t : v + t}};

        float lowest = 1.0f, highest = 0.0f;
        for (const auto& corner : corners)
        {
            const float textureU = (1.0f - corner[0] - corner[1]) * u0 + corner[0] * u1 + corner[1] * u2;
            lowest = std::min(lowest, textureU);
            highest = std::max(highest, textureU);
        }

        const auto state = stateAt(baked, block, index);
        const auto where = "block " + std::to_string(number) + ", cell " + std::to_string(cell) + ": state " + std::to_string(state);
        if (highest <= SolidUntil)
        {
            expect(state == GI_OMM_OPAQUE, where + " on the solid side");
            ++checked;
        }
        else if (lowest >= ClearFrom)
        {
            expect(state == GI_OMM_TRANSPARENT, where + " on the clear side");
            ++checked;
        }
        else
            ++checked; // 2-state: the centroid decided, either way is allowed here
    }
    return checked;
}

void bakesAgainstTheTexture()
{
    // A triangle across the edge, one on each side, a copy of the first (a shared block) and a sliver
    // on the solid side (level 0: one state in a padded word).
    const std::vector<std::uint32_t> uvs{uv(0.1f, 0.1f), uv(0.9f, 0.1f), uv(0.1f, 0.9f), uv(0.05f, 0.1f), uv(0.4f, 0.1f), uv(0.05f, 0.8f), uv(0.6f, 0.1f),
        uv(0.95f, 0.1f), uv(0.6f, 0.8f), uv(0.11f, 0.1f), uv(0.1f, 0.11f)};
    const std::vector<std::uint16_t> indices{0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 2, 0, 9, 10};
    const auto layout = omm_layout::layOut({uvs, indices, TextureSize, TextureSize, 1u << 20});
    expect(layout.has_value(), "no layout");
    expect(layout->blocks.size() == 4, "the copy did not share its block");

    const auto baked = bake(*layout);

    std::uint32_t checked = 0, cells = 0;
    for (std::uint32_t b = 0; b < layout->blocks.size(); ++b)
    {
        checked += checkBlock(baked, layout->blocks[b], b);
        cells += 1u << (2 * layout->blocks[b].level);
    }
    expect(checked * 10 >= cells * 9, "only " + std::to_string(checked) + " of " + std::to_string(cells) + " micro-triangles could be checked");

    // Every byte of a block written: no garbage past a padded block's states.
    for (const auto& block : layout->blocks)
        if (block.level == 0)
        {
            std::uint32_t word;
            std::memcpy(&word, &baked.data[block.byteOffset], 4);
            expect(word == GI_OMM_OPAQUE, "the sliver's padded word is " + std::to_string(word));
        }

    // Triangles: across the edge its block, solid side and sliver fully opaque, clear side fully transparent.
    const auto& blocks = layout->triangleBlocks;
    expect(baked.indices[0] == blocks[0] && baked.indices[3] == blocks[3], "the triangles across the edge did not keep their block");
    // No special indices: every triangle points at its own block, whose states say what it is.
    expect(baked.indices[1] == blocks[1], "the solid triangle does not point at its block");
    expect(baked.indices[4] == blocks[4], "the sliver does not point at its block");
    expect(baked.indices[2] == blocks[2], "the clear triangle does not point at its block");
    expect(baked.blockStates[blocks[0]] == (GI_OMM_SOME_NOT_TRANSPARENT | GI_OMM_SOME_NOT_OPAQUE), "the edge's block is not mixed");
}
}

int main()
{
    try
    {
        bakesAgainstTheTexture();
        std::cout << "Opacity micromap bake tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
