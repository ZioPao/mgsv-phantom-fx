// NRD's RELAX on Direct3D 12; gi_denoiser.hpp describes it.
#include "features/global_illumination/gi_denoiser.hpp"

#include "passes/ray_tracing/gi_scene.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <NRD.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace phantom_fx::gi_denoiser
{
namespace
{
using gi_d3d12::check;
using gi_d3d12::ComPtr;

constexpr nrd::Identifier Diffuse = 0;
// The most dispatches a frame records (RELAX's diffuse SH denoiser records about a dozen).
constexpr UINT MaxDispatches = 48;
// Half a second of history, as NRD recommends for RELAX; a fast history of a tenth of that, which the
// long one is clamped to, so the light follows a change within a few frames.
constexpr float AccumulationSeconds = 0.5f, FastAccumulationSeconds = 0.1f;
constexpr std::uint32_t HistoryFixFrames = 3;
// Pixels nearer than this are denoised; the sky's view depth lies beyond (gi_light.hlsl).
constexpr float DenoisingRange = GI_LIGHT_DENOISING_RANGE;

void checkNrd(nrd::Result result, const char* what)
{
    if (result != nrd::Result::SUCCESS)
        throw std::runtime_error(what + std::string(" failed (NRD ") + std::to_string(static_cast<std::uint32_t>(result)) + ").");
}

DXGI_FORMAT dxgiOf(nrd::Format format)
{
    switch (format)
    {
    case nrd::Format::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case nrd::Format::R8_SNORM: return DXGI_FORMAT_R8_SNORM;
    case nrd::Format::R8_UINT: return DXGI_FORMAT_R8_UINT;
    case nrd::Format::R8_SINT: return DXGI_FORMAT_R8_SINT;
    case nrd::Format::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case nrd::Format::RG8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
    case nrd::Format::RG8_UINT: return DXGI_FORMAT_R8G8_UINT;
    case nrd::Format::RG8_SINT: return DXGI_FORMAT_R8G8_SINT;
    case nrd::Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
    case nrd::Format::RGBA8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
    case nrd::Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case nrd::Format::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case nrd::Format::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
    case nrd::Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
    case nrd::Format::R16_SINT: return DXGI_FORMAT_R16_SINT;
    case nrd::Format::R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
    case nrd::Format::RG16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
    case nrd::Format::RG16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
    case nrd::Format::RG16_UINT: return DXGI_FORMAT_R16G16_UINT;
    case nrd::Format::RG16_SINT: return DXGI_FORMAT_R16G16_SINT;
    case nrd::Format::RG16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case nrd::Format::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
    case nrd::Format::RGBA16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
    case nrd::Format::RGBA16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case nrd::Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
    case nrd::Format::R32_SINT: return DXGI_FORMAT_R32_SINT;
    case nrd::Format::R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
    case nrd::Format::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
    case nrd::Format::RG32_SINT: return DXGI_FORMAT_R32G32_SINT;
    case nrd::Format::RG32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case nrd::Format::RGB32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
    case nrd::Format::RGB32_SINT: return DXGI_FORMAT_R32G32B32_SINT;
    case nrd::Format::RGB32_SFLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case nrd::Format::RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
    case nrd::Format::RGBA32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
    case nrd::Format::RGBA32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case nrd::Format::R10_G10_B10_A2_UINT: return DXGI_FORMAT_R10G10B10A2_UINT;
    case nrd::Format::R11_G11_B10_UFLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case nrd::Format::R9_G9_B9_E5_UFLOAT: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    default: throw std::runtime_error("NRD asked for a texture format the denoiser does not know.");
    }
}

std::vector<ComPtr<ID3D12Resource>> createPool(ID3D12Device* device, const nrd::TextureDesc* descs, std::uint32_t count, UINT width, UINT height)
{
    std::vector<ComPtr<ID3D12Resource>> pool;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const UINT factor = descs[i].downsampleFactor;
        pool.push_back(gi_d3d12::createTexture(device, (width + factor - 1) / factor, (height + factor - 1) / factor, dxgiOf(descs[i].format), 1,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "Creating the denoiser's textures"));
    }

    return pool;
}
}

Denoiser::Denoiser(ID3D12Device* device, UINT width, UINT height) : device(device), width(width), height(height)
{
    const nrd::DenoiserDesc denoisers[]{{Diffuse, nrd::Denoiser::RELAX_DIFFUSE_SH}};
    nrd::InstanceCreationDesc creation{};
    creation.denoisers = denoisers;
    creation.denoisersNum = static_cast<std::uint32_t>(std::size(denoisers));
    checkNrd(nrd::CreateInstance(creation, instance), "Creating the denoiser");

    const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*instance);
    // At least one of each: a descriptor range cannot be empty.
    texturesPerSet = std::max(desc.descriptorPoolDesc.perSetTexturesMaxNum, 1u);
    storagePerSet = std::max(desc.descriptorPoolDesc.perSetStorageTexturesMaxNum, 1u);

    // One layout for every pipeline: the constants at the root, the textures then the storage textures
    // in one table, and NRD's two samplers.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = texturesPerSet;
    ranges[0].BaseShaderRegister = desc.resourcesBaseRegisterIndex;
    ranges[0].RegisterSpace = desc.resourcesSpaceIndex;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = storagePerSet;
    ranges[1].BaseShaderRegister = desc.resourcesBaseRegisterIndex;
    ranges[1].RegisterSpace = desc.resourcesSpaceIndex;
    ranges[1].OffsetInDescriptorsFromTableStart = texturesPerSet;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = desc.constantBufferRegisterIndex;
    parameters[0].Descriptor.RegisterSpace = desc.constantBufferAndSamplersSpaceIndex;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(std::size(ranges));
    parameters[1].DescriptorTable.pDescriptorRanges = ranges;

    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(desc.samplersNum);
    for (std::uint32_t i = 0; i < desc.samplersNum; ++i)
    {
        auto& sampler = samplers[i];
        sampler.Filter = desc.samplers[i] == nrd::Sampler::NEAREST_CLAMP ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = desc.samplersBaseRegisterIndex + i;
        sampler.RegisterSpace = desc.constantBufferAndSamplersSpaceIndex;
    }

    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = static_cast<UINT>(std::size(parameters));
    signature.pParameters = parameters;
    signature.NumStaticSamplers = static_cast<UINT>(samplers.size());
    signature.pStaticSamplers = samplers.data();
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), errors.GetAddressOf())))
        throw std::runtime_error("Serializing the denoiser's root signature failed"
            + (errors ? ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string(".")));
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)),
        "Creating the denoiser's root signature");

    for (std::uint32_t i = 0; i < desc.pipelinesNum; ++i)
    {
        const auto& shader = desc.pipelines[i].computeShaderDXIL;
        if (!shader.bytecode) throw std::runtime_error("The denoiser has no DXIL for one of its pipelines.");

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = rootSignature.Get();
        pipeline.CS = {shader.bytecode, static_cast<SIZE_T>(shader.size)};
        check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipelines.emplace_back())), "Creating the denoiser's pipelines");
    }

    permanentPool = createPool(device, desc.permanentPool, desc.permanentPoolSize, width, height);
    transientPool = createPool(device, desc.transientPool, desc.transientPoolSize, width, height);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = gi_scene::FramesInFlight * MaxDispatches * (ranges[0].NumDescriptors + ranges[1].NumDescriptors);
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "Creating the denoiser's descriptor heap");
    descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    constantStride = static_cast<std::uint32_t>(gi_d3d12::aligned(desc.constantBufferMaxDataSize, 256));
    constants = gi_d3d12::createBuffer(device, std::uint64_t{gi_scene::FramesInFlight} * MaxDispatches * constantStride, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, "Creating the denoiser's constants");
    void* data{};
    check(constants->Map(0, nullptr, &data), "Mapping the denoiser's constants");
    constantData = static_cast<std::uint8_t*>(data);
}

Denoiser::~Denoiser()
{
    if (instance) nrd::DestroyInstance(*instance);
}

D3D12_CPU_DESCRIPTOR_HANDLE Denoiser::cpuAt(UINT index) const
{
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += std::size_t{index} * descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE Denoiser::gpuAt(UINT index) const
{
    auto handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += std::uint64_t{index} * descriptorSize;
    return handle;
}

void Denoiser::record(ID3D12GraphicsCommandList4& list, const Camera& camera, const Textures& textures, std::uint32_t frameSlot, float framesPerSecond)
{
    nrd::CommonSettings common{};
    std::memcpy(common.viewToClipMatrix, camera.viewToClip.data(), sizeof(common.viewToClipMatrix));
    std::memcpy(common.viewToClipMatrixPrev, camera.previousViewToClip.data(), sizeof(common.viewToClipMatrixPrev));
    std::memcpy(common.worldToViewMatrix, camera.worldToView.data(), sizeof(common.worldToViewMatrix));
    std::memcpy(common.worldToViewMatrixPrev, camera.previousWorldToView.data(), sizeof(common.worldToViewMatrixPrev));
    for (int i = 0; i < 2; ++i)
    {
        common.cameraJitter[i] = std::clamp(camera.jitter[i], -0.5f, 0.5f);
        common.cameraJitterPrev[i] = std::clamp(camera.previousJitter[i], -0.5f, 0.5f);
    }
    // 2.5D motion: uv, and view z's change.
    common.motionVectorScale[0] = common.motionVectorScale[1] = common.motionVectorScale[2] = 1.0f;
    common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] = static_cast<std::uint16_t>(width);
    common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] = static_cast<std::uint16_t>(height);
    common.denoisingRange = DenoisingRange;
    // The game's foliage as NRD's strands, which keep their history through far more depth change, most
    // while the camera is still: the game sways its grass and bushes without velocity, so the denoiser
    // looked for their history where they no longer were, and they shimmered (2026-10-25).
    common.strandMaterialID = GI_LIGHT_FOLIAGE_MATERIAL;
    common.strandThickness = 0.01f; // A grass blade's width; NRD's is a hair's
    common.frameIndex = frameIndex;
    common.accumulationMode = frameIndex == 0 ? nrd::AccumulationMode::CLEAR_AND_RESTART
        : camera.restart                      ? nrd::AccumulationMode::RESTART
                                              : nrd::AccumulationMode::CONTINUE;
    checkNrd(nrd::SetCommonSettings(*instance, common), "Setting the denoiser up");
    ++frameIndex;

    // NRD: maxAccumulated > fast > history fix.
    const float fps = std::clamp(framesPerSecond, 10.0f, 240.0f);
    nrd::RelaxSettings relax{};
    relax.diffuseMaxFastAccumulatedFrameNum = std::max(nrd::GetMaxAccumulatedFrameNum(FastAccumulationSeconds, fps), HistoryFixFrames + 1);
    relax.diffuseMaxAccumulatedFrameNum = std::min(std::max(nrd::GetMaxAccumulatedFrameNum(AccumulationSeconds, fps),
        relax.diffuseMaxFastAccumulatedFrameNum + 1), nrd::RELAX_MAX_HISTORY_FRAME_NUM);
    relax.historyFixFrameNum = HistoryFixFrames;
    // The hits' sunlight makes rare bright samples in the shade.
    relax.enableAntiFirefly = true;
    checkNrd(nrd::SetDenoiserSettings(*instance, Diffuse, &relax), "Setting the denoiser's settings");

    const nrd::DispatchDesc* dispatches{};
    std::uint32_t dispatchCount{};
    checkNrd(nrd::GetComputeDispatches(*instance, &Diffuse, 1, dispatches, dispatchCount), "Recording the denoiser");
    if (dispatchCount > MaxDispatches) throw std::runtime_error("The denoiser recorded more dispatches than it has room for.");

    const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*instance);
    const auto external = [&](nrd::ResourceType type) -> ID3D12Resource* {
        switch (type)
        {
        case nrd::ResourceType::IN_MV: return textures.motion;
        case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return textures.normalRoughness;
        case nrd::ResourceType::IN_VIEWZ: return textures.viewZ;
        case nrd::ResourceType::IN_DIFF_SH0: return textures.light0;
        case nrd::ResourceType::IN_DIFF_SH1: return textures.light1;
        case nrd::ResourceType::OUT_DIFF_SH0: return textures.denoised0;
        case nrd::ResourceType::OUT_DIFF_SH1: return textures.denoised1;
        default: throw std::runtime_error(std::string("The denoiser asked for ") + nrd::GetResourceTypeString(type) + ", which it is not given.");
        }
    };

    ID3D12DescriptorHeap* heaps[]{heap.Get()};
    list.SetDescriptorHeaps(1, heaps);
    list.SetComputeRootSignature(rootSignature.Get());

    const UINT setSize = texturesPerSet + storagePerSet;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    // A dispatch without constants of its own keeps the last one's.
    std::uint64_t constantOffset = std::uint64_t{frameSlot} * MaxDispatches * constantStride;
    for (std::uint32_t d = 0; d < dispatchCount; ++d)
    {
        const auto& dispatch = dispatches[d];
        const auto& pipeline = desc.pipelines[dispatch.pipelineIndex];
        const UINT set = (frameSlot * MaxDispatches + d) * setSize;

        // Null views first: a pipeline reads only its own, but every slot of the table holds a view.
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
        nullSrv.Format = DXGI_FORMAT_R8_UNORM;
        nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
        nullUav.Format = DXGI_FORMAT_R8_UNORM;
        nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < texturesPerSet; ++i) device->CreateShaderResourceView(nullptr, &nullSrv, cpuAt(set + i));
        for (UINT i = 0; i < storagePerSet; ++i)
            device->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, cpuAt(set + texturesPerSet + i));

        // The dispatch's resources come range by range: textures, then storage textures.
        barriers.clear();
        barriers.push_back(gi_d3d12::uavBarrier());
        std::uint32_t n = 0;
        for (std::uint32_t r = 0; r < pipeline.resourceRangesNum; ++r)
        {
            const auto& range = pipeline.resourceRanges[r];
            const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            for (std::uint32_t j = 0; j < range.descriptorsNum; ++j, ++n)
            {
                const auto& resource = dispatch.resources[n];
                ID3D12Resource* texture = resource.type == nrd::ResourceType::PERMANENT_POOL ? permanentPool[resource.indexInPool].Get()
                    : resource.type == nrd::ResourceType::TRANSIENT_POOL                     ? transientPool[resource.indexInPool].Get()
                                                                                             : external(resource.type);
                const auto wanted = storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                const auto state = states.try_emplace(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS).first;
                if (state->second != wanted)
                {
                    barriers.push_back(gi_d3d12::transition(texture, state->second, wanted));
                    state->second = wanted;
                }

                const auto format = texture->GetDesc().Format;
                if (storage)
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                    uav.Format = format;
                    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    device->CreateUnorderedAccessView(texture, nullptr, &uav, cpuAt(set + texturesPerSet + j));
                }
                else
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.Format = format;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.Texture2D.MipLevels = 1;
                    device->CreateShaderResourceView(texture, &srv, cpuAt(set + j));
                }
            }
        }

        list.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        if (dispatch.constantBufferDataSize && !dispatch.constantBufferDataMatchesPreviousDispatch)
        {
            constantOffset = std::uint64_t{frameSlot * MaxDispatches + d} * constantStride;
            std::memcpy(constantData + constantOffset, dispatch.constantBufferData, dispatch.constantBufferDataSize);
        }
        list.SetPipelineState(pipelines[dispatch.pipelineIndex].Get());
        list.SetComputeRootConstantBufferView(0, constants->GetGPUVirtualAddress() + constantOffset);
        list.SetComputeRootDescriptorTable(1, gpuAt(set));
        list.Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
    }

    // Everything back to the unordered access state for the next frame and the caller.
    barriers.clear();
    barriers.push_back(gi_d3d12::uavBarrier());
    for (auto& [texture, state] : states)
        if (state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            barriers.push_back(gi_d3d12::transition(texture, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

    list.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}
}
