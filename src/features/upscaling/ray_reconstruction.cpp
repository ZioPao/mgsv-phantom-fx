// DLSS Ray Reconstruction on Direct3D 12 for the Direct3D 11 game; ray_reconstruction.hpp describes the route.
#include "features/upscaling/ray_reconstruction.hpp"

#include "d3d12_helpers.hpp"
#include "features/frame_generation/frame_generation.hpp"
#include "hresult.hpp"
#include "log.hpp"

#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace phantom_fx::ray_reconstruction
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr std::size_t ListsInFlight = 3;

// What the feature was created for.
struct FeatureKey
{
    UINT renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    int quality{};
    upscaler::ReconstructionPreset preset{};
    bool hdr{};

    bool operator==(const FeatureKey&) const = default;
};

struct State
{
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Fence> fence11;

    // Native, as FSR's (fsr_upscaler.cpp): other add-ons would see ReShade's proxy as a game device.
    ComPtr<ID3D12Device> wrapper, device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence12;
    std::array<ComPtr<ID3D12CommandAllocator>, ListsInFlight> allocators;
    std::array<UINT64, ListsInFlight> submitted{};
    ComPtr<ID3D12GraphicsCommandList> list;
    std::unique_ptr<void, HandleCloser> event;
    UINT64 fenceValue{}, lists{};

    bool initialized{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    FeatureKey key;
    OpenedTexture color, depth, motion, output, normalRoughness, diffuseAlbedo, specularAlbedo, dofGuide, specularHitDistance;
};
std::unique_ptr<State> state;

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::string text(message ? message : "");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    log("NGX (Direct3D 12): " + text);
}

ID3D12Resource* open(State& s, OpenedTexture& shared, ID3D11Texture2D* texture, const char* what)
{
    check(shared.open(s.device12.Get(), texture), what);
    return shared.resource.Get();
}

void waitFor(State& s, UINT64 value)
{
    if (s.fence12->GetCompletedValue() >= value) return;

    check(s.fence12->SetEventOnCompletion(value, s.event.get()), "Waiting for Direct3D 12");
    if (WaitForSingleObject(s.event.get(), FenceTimeout) != WAIT_OBJECT_0)
        throw std::runtime_error("Direct3D 12 did not finish Ray Reconstruction within 10 seconds.");
}

void releaseFeature(State& s)
{
    if (!s.feature) return;

    // The feature's resources may still be in use by the last evaluation.
    waitFor(s, s.fenceValue);
    NVSDK_NGX_D3D12_ReleaseFeature(s.feature);
    s.feature = nullptr;
}

// Throws with a message for the player when Ray Reconstruction cannot run here.
void initializeNgx(State& s, const std::filesystem::path& directory)
{
    const auto modelDirectory = directory.wstring();
    const wchar_t* paths[]{modelDirectory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    auto result = NVSDK_NGX_D3D12_Init_with_ProjectID(frame_generation::NgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Fox Engine 1.0.15",
        directory.c_str(), s.device12.Get(), &info);
    if (NVSDK_NGX_FAILED(result)) throw std::runtime_error("NVIDIA NGX could not start on Direct3D 12 (" + hex(static_cast<std::uint32_t>(result)) + ").");

    s.initialized = true;
    int available{};
    result = NVSDK_NGX_D3D12_GetCapabilityParameters(&s.parameters);
    if (NVSDK_NGX_SUCCEED(result)) NVSDK_NGX_Parameter_GetI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
    if (!available)
    {
        int needsDriver{};
        unsigned major{}, minor{};
        if (s.parameters)
        {
            NVSDK_NGX_Parameter_GetI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsDriver);
            NVSDK_NGX_Parameter_GetUI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &major);
            NVSDK_NGX_Parameter_GetUI(s.parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &minor);
        }

        throw std::runtime_error(needsDriver ? "Ray Reconstruction needs NVIDIA driver " + std::to_string(major) + "." + std::to_string(minor) + " or newer."
                                             : std::string("Ray Reconstruction is unavailable on this GPU. nvngx_dlssd.dll must be beside the add-on."));
    }
}

// The feature keeps its history at the sizes it was created for. Records its creation into the open
// list; returns true when it made one.
bool ensureFeature(State& s, const FeatureKey& key)
{
    if (s.feature && s.key == key) return false;

    releaseFeature(s);
    for (const char* hint : {NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(s.parameters, hint, static_cast<unsigned>(key.preset));

    NVSDK_NGX_DLSSD_Create_Params create{};
    create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
    create.InWidth = key.renderWidth;
    create.InHeight = key.renderHeight;
    create.InTargetWidth = key.outputWidth;
    create.InTargetHeight = key.outputHeight;
    create.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(key.quality);
    // The motion vectors are per render pixel; reversed-Z depth; linear light.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (key.hdr) create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    const auto result = NGX_D3D12_CREATE_DLSSD_EXT(s.list.Get(), 1, 1, &s.feature, s.parameters, &create);
    if (NVSDK_NGX_FAILED(result))
    {
        s.feature = nullptr;
        throw std::runtime_error("Creating the Ray Reconstruction feature failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
    }

    s.key = key;
    log("Created DLSS Ray Reconstruction on Direct3D 12: " + std::to_string(key.renderWidth) + "x" + std::to_string(key.renderHeight) + " -> "
        + std::to_string(key.outputWidth) + "x" + std::to_string(key.outputHeight) + ", preset " + std::string(upscaler::label(key.preset)) + ".");

    return true;
}

UINT64 submit(State& s, UINT64 waitValue)
{
    check(s.list->Close(), "Recording Ray Reconstruction");
    check(s.queue->Wait(s.fence12.Get(), waitValue), "Waiting for the Ray Reconstruction inputs");
    ID3D12CommandList* lists[]{s.list.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    const auto done = ++s.fenceValue;
    check(s.queue->Signal(s.fence12.Get(), done), "Signalling the Ray Reconstruction output");

    return done;
}
}

void start(ID3D11Device* device, const std::filesystem::path& directory)
{
    if (state) return;

    auto s = std::make_unique<State>();
    ComPtr<ID3D11Device5> device11;
    check(device->QueryInterface(IID_PPV_ARGS(&device11)), "Finding Direct3D 11.4");
    ComPtr<ID3D11DeviceContext> context;
    device11->GetImmediateContext(context.GetAddressOf());
    check(context.As(&s->context11), "Finding the Direct3D 11.4 context");
    check(device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s->fence11)), "Creating the shared fence");

    const auto d3d12 = createDevice12(device11.Get());
    s->wrapper = d3d12.wrapper;
    s->device12 = d3d12.native;

    const D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_DIRECT};
    check(s->device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&s->queue)), "Creating the Direct3D 12 queue");
    for (auto& allocator : s->allocators)
        check(s->device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Creating a command allocator");

    check(s->device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->allocators[0].Get(), nullptr, IID_PPV_ARGS(&s->list)),
        "Creating the command list");
    check(s->list->Close(), "Closing the command list");
    HANDLE fence{};
    check(s->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence), "Sharing the fence");
    check(openSharedHandle(s->device12.Get(), fence, s->fence12), "Opening the shared fence");
    s->event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!s->event) throw std::runtime_error("Creating the fence event failed.");

    try
    {
        initializeNgx(*s, directory);
    }
    catch (...)
    {
        if (s->parameters) NVSDK_NGX_D3D12_DestroyParameters(s->parameters);
        if (s->initialized) NVSDK_NGX_D3D12_Shutdown1(s->device12.Get());
        throw;
    }

    state = std::move(s);
    log("Ray Reconstruction started on Direct3D 12.");
}

bool started() noexcept
{
    return state != nullptr;
}

bool evaluate(const Inputs& inputs)
{
    auto& s = *state;
    D3D11_TEXTURE2D_DESC color{}, output{};
    inputs.color->GetDesc(&color);
    inputs.output->GetDesc(&output);

    // Direct3D 12 starts once Direct3D 11 has drawn the inputs.
    const auto drawn = ++s.fenceValue;
    check(s.context11->Signal(s.fence11.Get(), drawn), "Signalling the Ray Reconstruction inputs");
    s.context11->Flush();

    const auto slot = s.lists++ % ListsInFlight;
    waitFor(s, s.submitted[slot]);
    check(s.allocators[slot]->Reset(), "Resetting a command allocator");
    check(s.list->Reset(s.allocators[slot].Get(), nullptr), "Resetting the command list");
    bool created{};
    try
    {
        created = ensureFeature(s, {color.Width, color.Height, output.Width, output.Height, inputs.quality, inputs.preset, inputs.hdr});
        ID3D12Resource* reads[]{open(s, s.color, inputs.color, "Opening the Ray Reconstruction colour"),
            open(s, s.depth, inputs.depth, "Opening the Ray Reconstruction depth"), open(s, s.motion, inputs.motion, "Opening the Ray Reconstruction motion"),
            open(s, s.normalRoughness, inputs.normalRoughness, "Opening the Ray Reconstruction normals"),
            open(s, s.diffuseAlbedo, inputs.diffuseAlbedo, "Opening the Ray Reconstruction diffuse albedo"),
            open(s, s.specularAlbedo, inputs.specularAlbedo, "Opening the Ray Reconstruction specular albedo")};
        auto* dofGuide = inputs.dofGuide ? open(s, s.dofGuide, inputs.dofGuide, "Opening the Ray Reconstruction depth of field guide") : nullptr;
        auto* written = open(s, s.output, inputs.output, "Opening the Ray Reconstruction output");
        auto* hitDistance = inputs.specularHitDistance
            ? open(s, s.specularHitDistance, inputs.specularHitDistance, "Opening the Ray Reconstruction specular hit distance")
            : nullptr;
        std::array<D3D12_RESOURCE_BARRIER, std::size(reads) + 3> barriers{};
        UINT barrierCount{};
        for (auto* read : reads) barriers[barrierCount++] = transition(read, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (dofGuide) barriers[barrierCount++] = transition(dofGuide, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (hitDistance) barriers[barrierCount++] = transition(hitDistance, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers[barrierCount++] = transition(written, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s.list->ResourceBarrier(barrierCount, barriers.data());

        // NGX keeps the matrices' pointers only for the call.
        auto worldToView = inputs.worldToView;
        auto viewToClip = inputs.viewToClip;
        NVSDK_NGX_D3D12_DLSSD_Eval_Params evaluation{};
        evaluation.pInColor = reads[0];
        evaluation.pInDepth = reads[1];
        evaluation.pInMotionVectors = reads[2];
        evaluation.pInNormals = reads[3];
        evaluation.pInDiffuseAlbedo = reads[4];
        evaluation.pInSpecularAlbedo = reads[5];
        evaluation.pInDepthOfFieldGuide = dofGuide;
        evaluation.pInOutput = written;
        evaluation.pInSpecularHitDistance = hitDistance;
        evaluation.InJitterOffsetX = inputs.jitter[0];
        evaluation.InJitterOffsetY = inputs.jitter[1];
        evaluation.InRenderSubrectDimensions = {color.Width, color.Height};
        evaluation.InReset = inputs.reset || created ? 1 : 0;
        evaluation.InMVScaleX = 1;
        evaluation.InMVScaleY = 1;
        evaluation.pInWorldToViewMatrix = worldToView.data();
        evaluation.pInViewToClipMatrix = viewToClip.data();
        evaluation.InFrameTimeDeltaInMsec = inputs.frameTime;
        const auto result = NGX_D3D12_EVALUATE_DLSSD_EXT(s.list.Get(), s.feature, s.parameters, &evaluation);
        if (NVSDK_NGX_FAILED(result)) throw std::runtime_error("Ray Reconstruction failed (" + hex(static_cast<std::uint32_t>(result)) + ").");

        for (UINT i = 0; i < barrierCount; ++i) std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
        s.list->ResourceBarrier(barrierCount, barriers.data());
    }
    catch (...)
    {
        // The next frame resets the list, which it can only once closed.
        s.list->Close();
        throw;
    }

    const auto reconstructed = submit(s, drawn);
    s.submitted[slot] = reconstructed;

    // The composite reads the output.
    check(s.context11->Wait(s.fence11.Get(), reconstructed), "Waiting for Ray Reconstruction");

    return created;
}

void release() noexcept
{
    if (!state || !state->feature) return;

    try
    {
        releaseFeature(*state);
        log("Released DLSS Ray Reconstruction.");
    }
    catch (const std::exception& error)
    {
        log(error.what());
    }
}

void stop() noexcept
{
    if (!state) return;

    auto& s = *state;
    release();

    if (s.parameters) NVSDK_NGX_D3D12_DestroyParameters(s.parameters);
    if (s.initialized) NVSDK_NGX_D3D12_Shutdown1(s.device12.Get());
    state.reset();
    log("Ray Reconstruction stopped.");
}
}
