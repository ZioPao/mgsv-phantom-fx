// FSR frame generation through AMD's swap chain; fsr_frame_generation.hpp describes the route.
#include "fsr_frame_generation.hpp"

#include "d3d12_helpers.hpp"
#include "fidelityfx.hpp"
#include "log.hpp"

#include <wrl/client.h>
#include <api/include/dx12/ffx_api_dx12.h>
#include <framegeneration/include/ffx_framegeneration.h>
#include <framegeneration/include/dx12/ffx_api_framegeneration_dx12.h>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace phantom_fx::fsr_frame_generation
{
namespace
{
using Microsoft::WRL::ComPtr;

// What the generation context was created for.
struct ContextKey
{
    UINT width{}, height{};
    DXGI_FORMAT backBuffer{}, hudless{};
    bool hdr{};

    bool operator==(const ContextKey&) const = default;
};

const fidelityfx::Api* api{};
std::atomic<bool> wanted{};

// Everything below is used on the presenting thread, and by the game's swap chain calls.
std::mutex stateMutex;
ffxContext swapChainContext{}, generationContext{};
IDXGISwapChain4* swapChain{}; // AMD's, owned by the presenter
ContextKey key;
// FSR generates at present, after Direct3D 11 may have drawn into the shared hudless texture again,
// so it reads a copy. Kept readable by compute between frames.
ComPtr<ID3D12Resource> hudlessCopy;
bool generating{}, hdrOutput{};
std::string version, failure;
LARGE_INTEGER lastFrame{};

// The SDR hudless texture is typeless, so its sRGB view can draw it (upscaler.cpp, "hudless"); FSR
// reads its copy as the real swap chain holds the bits.
DXGI_FORMAT typed(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : format;
}

// AMD's swap chain calls this from its Present to generate the frame. HDR output is PQ.
ffxReturnCode_t generate(ffxDispatchDescFrameGeneration* dispatch, void* context)
{
    if (hdrOutput) dispatch->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;

    return api->dispatch(static_cast<ffxContext*>(context), &dispatch->header);
}

void configure(bool on, std::uint64_t frame)
{
    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = swapChain;
    config.frameGenerationCallback = &generate;
    config.frameGenerationCallbackUserContext = &generationContext;
    config.frameGenerationEnabled = on;
    if (on) config.HUDLessColor = ffxApiGetResourceDX12(hudlessCopy.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    config.generationRect = {0, 0, static_cast<int32_t>(key.width), static_cast<int32_t>(key.height)};
    config.frameID = frame;
    fidelityfx::check(api->configure(&generationContext, &config.header), "Configuring FSR frame generation");
    generating = on;
}

void destroyGeneration()
{
    if (!generationContext) return;

    // FSR wants generation off before its context goes.
    try
    {
        configure(false, 0);
    }
    catch (const std::exception& error)
    {
        logOnce(error.what());
    }

    api->destroyContext(&generationContext, nullptr);
    generationContext = nullptr;
    generating = false;
    hudlessCopy.Reset();
}

// FSR generates at the back buffer's size and format, so a resize or HDR needs a new context.
void ensureGeneration(ID3D12Resource* hudless, bool hdr)
{
    DXGI_SWAP_CHAIN_DESC1 chain{};
    if (FAILED(swapChain->GetDesc1(&chain))) throw std::runtime_error("Reading the swap chain's format failed.");

    auto copy = hudless->GetDesc();
    copy.Format = typed(copy.Format);
    copy.Flags = D3D12_RESOURCE_FLAG_NONE;
    const ContextKey needed{chain.Width, chain.Height, chain.Format, copy.Format, hdr};
    if (generationContext && key == needed) return;

    destroyGeneration();
    ComPtr<ID3D12Device> device;
    if (FAILED(hudless->GetDevice(IID_PPV_ARGS(&device)))) throw std::runtime_error("Finding the Direct3D 12 device failed.");

    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    // Fox Engine's projection is reversed-Z with a finite far plane.
    create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (hdr) create.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    create.displaySize = {needed.width, needed.height};
    create.maxRenderSize = {needed.width, needed.height};
    create.backBufferFormat = ffxApiGetSurfaceFormatDX12(needed.backBuffer);
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device.Get();
    ffxCreateContextDescFrameGenerationHudless hudlessFormat{};
    hudlessFormat.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudlessFormat.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(needed.hudless);
    ffxCreateContextDescFrameGenerationVersion versionDesc{};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    versionDesc.version = FFX_FRAMEGENERATION_VERSION;
    create.header.pNext = &backend.header;
    backend.header.pNext = &hudlessFormat.header;
    hudlessFormat.header.pNext = &versionDesc.header;
    fidelityfx::check(api->createContext(&generationContext, &create.header, nullptr), "Creating the FSR frame generation context");
    key = needed;

    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&hudlessCopy))))
        throw std::runtime_error("Creating the hudless copy for FSR frame generation failed.");

    version = fidelityfx::version(*api, generationContext);
    log("FSR frame generation " + version + " at " + std::to_string(needed.width) + "x" + std::to_string(needed.height) + (hdr ? ", HDR." : "."));
}

void prepare(ID3D12GraphicsCommandList* list, const frame_generation::Inputs& inputs, ID3D12Resource* depth, ID3D12Resource* motion,
    ID3D12Resource* hudless)
{
    D3D12_RESOURCE_BARRIER barriers[]{
        transition(hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(hudlessCopy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
    list->CopyResource(hudlessCopy.Get(), hudless);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    list->ResourceBarrier(1, &barriers[1]);

    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const auto& camera = inputs.camera;
    ffxDispatchDescFrameGenerationPrepareV2 dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    dispatch.frameID = inputs.frame;
    dispatch.commandList = list;
    dispatch.renderSize = {inputs.renderWidth, inputs.renderHeight};
    dispatch.jitterOffset = {inputs.jitter[0], inputs.jitter[1]};
    dispatch.motionVectorScale = {1, 1};
    dispatch.frameTimeDelta = lastFrame.QuadPart
        ? static_cast<float>(static_cast<double>(now.QuadPart - lastFrame.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart))
        : 16.7f;
    dispatch.reset = inputs.reset;
    dispatch.cameraNear = camera.nearPlane;
    dispatch.cameraFar = camera.farPlane;
    dispatch.cameraFovAngleVertical = camera.verticalFov;
    // Fox Engine's units are metres.
    dispatch.viewSpaceToMetersFactor = 1;
    dispatch.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    for (int axis = 0; axis < 3; ++axis)
    {
        dispatch.cameraPosition[axis] = camera.position[axis];
        dispatch.cameraRight[axis] = camera.right[axis];
        dispatch.cameraUp[axis] = camera.up[axis];
        dispatch.cameraForward[axis] = camera.forward[axis];
    }

    fidelityfx::check(api->dispatch(&generationContext, &dispatch.header), "Preparing FSR frame generation");
    lastFrame = now;

    D3D12_RESOURCE_BARRIER restore[]{barriers[0], barriers[2], barriers[3]};
    for (auto& barrier : restore) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(static_cast<UINT>(std::size(restore)), restore);
}
}

bool start(const std::filesystem::path& directory) noexcept
{
    try
    {
        api = &fidelityfx::load(directory);

        return true;
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(stateMutex);
        failure = error.what();

        return false;
    }
}

IDXGISwapChain4* createSwapChain(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND window, const DXGI_SWAP_CHAIN_DESC1& desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen)
{
    std::lock_guard lock(stateMutex);
    auto chainDesc = desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
    if (fullscreen) fullscreenDesc = *fullscreen;

    IDXGISwapChain4* created{};
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    create.swapchain = &created;
    create.hwnd = window;
    create.desc = &chainDesc;
    create.fullscreenDesc = fullscreen ? &fullscreenDesc : nullptr;
    create.dxgiFactory = factory;
    create.gameQueue = queue;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc{};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    create.header.pNext = &versionDesc.header;
    fidelityfx::check(api->createContext(&swapChainContext, &create.header, nullptr), "Creating FSR's swap chain");
    swapChain = created;
    failure.clear();

    return created;
}

void record(ID3D12GraphicsCommandList* list, const frame_generation::Inputs& inputs, ID3D12Resource* depth, ID3D12Resource* motion,
    ID3D12Resource* hudless, bool hdr) noexcept
{
    std::lock_guard lock(stateMutex);
    if (!swapChainContext) return;

    try
    {
        const bool on = wanted && inputs.valid && inputs.frame;
        hdrOutput = hdr;
        if (on)
        {
            ensureGeneration(hudless, hdr);
            prepare(list, inputs, depth, motion, hudless);
        }

        // Frames without upscaling (menus, loading) present as they are.
        if (generationContext && (on || generating)) configure(on, inputs.frame);
        if (on) failure.clear();
    }
    catch (const std::exception& error)
    {
        failure = error.what();
        logOnce(failure);
    }
}

void release() noexcept
{
    std::lock_guard lock(stateMutex);
    destroyGeneration();
}

void stop() noexcept
{
    std::lock_guard lock(stateMutex);
    destroyGeneration();
    if (swapChainContext) api->destroyContext(&swapChainContext, nullptr);
    swapChainContext = nullptr;
    swapChain = nullptr;
}

void select(bool on) noexcept
{
    wanted = on;
}

std::string status()
{
    std::lock_guard lock(stateMutex);
    if (!failure.empty()) return "FSR frame generation failed: " + failure;
    if (!wanted) return "Off.";
    if (!generating) return "Paused: upscaling is not running on this frame.";

    return "Generating with FSR " + version + ": 2 frames presented per rendered frame.";
}

unsigned presentedPerFrame() noexcept
{
    std::lock_guard lock(stateMutex);
    return failure.empty() && wanted && generating ? 2 : 1;
}
}
