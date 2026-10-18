#include "upscaler.hpp"

#include "camera.hpp"
#include "frame_rate.hpp"
#include "fxaa.hpp"
#include "log.hpp"
#include "motion_blur.hpp"
#include "render_scale.hpp"
#include "shader_bytecode.hpp"
#include "upscaler_shaders.hpp"

#include <Windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace mgsv_dlss::upscaler
{
namespace
{
using Microsoft::WRL::ComPtr;

// Any GUID identifies a custom-engine NGX project.
constexpr char NgxProjectId[] = "3f6c2a91-7d4e-4b8a-a1c5-9e0d2b7f4c68";
constexpr UINT SceneConstantsSize = sizeof(camera::SceneConstants);

// The game's pixel shaders the add-on looks for, each drawn as one full-screen triangle.
enum class Pass : std::uint8_t { Fxaa, CameraVelocity, GammaControl, Count, None = Count };

struct GameShader
{
    const char* name;
    const char* checksum; // docs/DEVELOPMENT.md, "Shaders"
};

constexpr GameShader Shaders[]{
    {"Fxaa", "deef1a5e3462ab091b5612a37662d15d"},
    {"MotionBlurCameraVelocity", "5d0241dd9804f41c780cd36669c71198"},
    {"GammaControl", "12790f061c5f1460aa40c264e4bc9f07"},
};
static_assert(std::size(Shaders) == static_cast<std::size_t>(Pass::Count));

// The FXAA pass as recorded: it reads the scene copy and writes the scene target, with the depth copy bound.
struct FxaaPass
{
    ComPtr<ID3D11ShaderResourceView> sceneCopy;
    ComPtr<ID3D11RenderTargetView> sceneTarget;
    ComPtr<ID3D11DepthStencilView> depth;
    std::uint64_t frame{};
};

// The engine's velocity for dynamic meshes, as MotionBlurCameraVelocity reads it.
struct ObjectVelocity
{
    ComPtr<ID3D11ShaderResourceView> view;
    std::uint64_t frame{};
};

// What a deferred context or command list recorded that the replacement needs.
struct Recorded
{
    std::optional<FxaaPass> fxaa;
    std::optional<ObjectVelocity> velocity;
    // Draws on the UI target, which must be filled before the list runs.
    bool drawsOnUiTarget{};
};

// The game records a whole frame, then executes it (docs/DEVELOPMENT.md, "Command lists"). A frame
// starts with the first main-view upload after the immediate context executed anything, and every
// main-view upload of the frame gets the same jitter.
struct Frame
{
    std::uint64_t index{};
    bool jittered{};
    std::array<float, 2> jitter{}; // Render pixels
    std::optional<camera::View> view;
};

struct Target
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> access;
    UINT width{}, height{};
};

struct InputConstants
{
    float previousFromCurrent[4][4];
    float jitterNdc[2];
    std::uint32_t renderSize[2];
    std::uint32_t objectMotion, padding[3];
};

struct CompositeConstants
{
    std::uint32_t debugView;
    float sharpness;
    std::uint32_t objectMotion, padding;
};

// The render size DLSS reads and the engine is asked for, and the output size it writes.
struct Plan
{
    UINT renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    NVSDK_NGX_PerfQuality_Value quality{NVSDK_NGX_PerfQuality_Value_DLAA};

    bool operator==(const Plan&) const = default;
};

struct FeatureKey
{
    Plan plan;
    Preset preset{};

    bool operator==(const FeatureKey&) const = default;
};

struct Ngx
{
    bool initialized{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    FeatureKey key{};
};

// ---- Shared state ----
std::filesystem::path dataDirectory, settingsPath;
bool supported{};
std::mutex settingsMutex;
Settings settings;
Diagnostics diagnostics;
std::optional<std::uint64_t> unsavedChange; // GetTickCount64 of the latest change MGSVDLSS.ini does not have yet

// Read on every recording thread: DLSS replaces FXAA this frame, and the next frame is jittered.
std::atomic<bool> replacing{}, jitterNext{};
// The current plan's jitter phases and texture LOD bias.
std::atomic<int> jitterPhases{8};
std::atomic<float> targetMipBias{};

// Biased copies of the game's material samplers, by the game's sampler and the bias; read on every
// recording thread.
struct BiasedSampler
{
    ComPtr<ID3D11SamplerState> original, biased;
};
std::shared_mutex samplerMutex;
std::map<std::pair<ID3D11SamplerState*, float>, BiasedSampler> biasedSamplers;

// Identified game shaders: read lock-free on every full-screen draw. The game can create a shader more
// than once, so each pass keeps every copy; holding them keeps their addresses from being reused.
constexpr std::size_t CopiesPerShader = 4;
std::mutex shaderMutex;
std::array<std::array<std::atomic<IUnknown*>, CopiesPerShader>, std::size(Shaders)> gameShaders{};
std::array<std::array<ComPtr<IUnknown>, CopiesPerShader>, std::size(Shaders)> shaderOwners;

// ---- Recording ----
std::mutex recordMutex;
std::unordered_map<std::uint64_t, Recorded> recordedLists;
Frame frame;
bool executedSinceFrameStart{true};
// The scene constants a thread has mapped: Map, memcpy and Unmap run back to back on one thread.
thread_local ID3D11Resource* mappedScene{};
thread_local void* mappedSceneData{};

// Set while the replacement issues its own Direct3D calls through the game's context.
thread_local int bypass{};

struct Bypass
{
    Bypass() { ++bypass; }
    ~Bypass() { --bypass; }
    Bypass(const Bypass&) = delete;
    Bypass& operator=(const Bypass&) = delete;
};

// ---- Execution, on the thread running the immediate context ----
std::mutex executionMutex;
ComPtr<ID3D11Device> device, ngxDevice;
ComPtr<ID3D11DeviceContext> immediate, ngxContext;
ComPtr<ID3D11DeviceContext1> context1;
ComPtr<ID3DDeviceContextState> ownState;
ComPtr<ID3D11ComputeShader> inputShader;
ComPtr<ID3D11VertexShader> fullScreenShader, stretchVertexShader;
ComPtr<ID3D11PixelShader> compositeShader, stretchShader;
ComPtr<ID3D11SamplerState> linearSampler;
ComPtr<ID3D11Buffer> inputConstants, compositeConstants;
ComPtr<ID3D11Texture2D> exposureTexture; // 1x1, the constant 1 DLSS is told as the exposure
Target colorTarget, depthTarget, motionTarget, outputTarget;
ComPtr<ID3D11Resource> depthResource;
ComPtr<ID3D11ShaderResourceView> depthView;
Ngx ngx;
UINT outputSize[2]{};
std::optional<Plan> plan;
Settings planSettings;
// The UI target holds this frame's image: the 2D pass and GammaControl can draw on it.
bool uiTargetFilled{};
// The FXAA pass of the command list just executed; DLSS runs at the immediate context's next call.
std::optional<FxaaPass> pending;
std::optional<ObjectVelocity> velocity;
std::optional<Frame> lastEvaluated;
LARGE_INTEGER lastEvaluation{};
std::atomic<bool> failed{};
std::string failure, lastFallback;
std::uint64_t presents{}, lastEvaluatedPresent{}, evaluations{}, fallbacks{}, resets{}, cameraCuts{};
std::map<std::string, bool, std::less<>> loggedOnce;

void logOnce(const std::string& key, std::string_view message)
{
    if (loggedOnce.emplace(key, true).second) log(message);
}

void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
}

void updateReplacing()
{
    replacing = supported && !failed && settings.mode != Mode::Off && !diagnostics.gameFxaa;
    motion_blur::setVelocityOnly(replacing);
    fxaa::setForced(replacing);
}

// The game keeps FXAA's output where DLSS cannot run.
bool fallback(std::string reason)
{
    ++fallbacks;
    logOnce("fallback:" + reason, "FXAA kept this frame: " + reason + ".");
    lastFallback = std::move(reason);
    return false;
}

void fail(std::string_view reason)
{
    failed = true;
    failure = reason;
    {
        std::lock_guard lock(settingsMutex);
        updateReplacing();
    }

    log("DLSS stopped: " + failure);
}

// ---- Shader identity ----

void remember(std::size_t pass, ComPtr<IUnknown> shader)
{
    std::lock_guard lock(shaderMutex);
    auto& copies = gameShaders[pass];
    for (std::size_t i = 0; i < CopiesPerShader; ++i)
    {
        if (copies[i].load()) continue;

        shaderOwners[pass][i] = std::move(shader);
        copies[i] = shaderOwners[pass][i].Get();
        log(std::string("Identified ") + Shaders[pass].name + ".");
        return;
    }
}

Pass passOf(IUnknown* shader)
{
    if (!shader) return Pass::None;

    for (std::size_t pass = 0; pass < std::size(Shaders); ++pass)
        for (const auto& copy : gameShaders[pass])
            if (copy.load(std::memory_order_relaxed) == shader) return static_cast<Pass>(pass);

    return Pass::None;
}

ComPtr<ID3D11Resource> resourceOf(ID3D11View* view)
{
    ComPtr<ID3D11Resource> resource;
    if (view) view->GetResource(resource.GetAddressOf());

    return resource;
}

D3D11_TEXTURE2D_DESC textureDesc(ID3D11View* view)
{
    D3D11_TEXTURE2D_DESC desc{};
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resourceOf(view).As(&texture))) texture->GetDesc(&desc);

    return desc;
}

// ---- Recording ----

std::uint64_t keyOf(ID3D11DeviceContext* context)
{
    return reinterpret_cast<std::uint64_t>(context);
}

// The frame a pass recorded now belongs to; call with recordMutex held. Between the immediate
// context's last call and the next frame's first upload, that is the next frame.
std::uint64_t recordingFrame()
{
    return executedSinceFrameStart ? frame.index + 1 : frame.index;
}

void recordFxaa(ID3D11DeviceContext* context)
{
    FxaaPass pass;
    context->PSGetShaderResources(0, 1, pass.sceneCopy.GetAddressOf());
    context->OMGetRenderTargets(1, pass.sceneTarget.GetAddressOf(), pass.depth.GetAddressOf());
    std::lock_guard lock(recordMutex);
    pass.frame = recordingFrame();
    recordedLists[keyOf(context)].fxaa = std::move(pass);
}

void recordVelocity(ID3D11DeviceContext* context)
{
    ObjectVelocity found;
    context->PSGetShaderResources(3, 1, found.view.GetAddressOf());
    std::lock_guard lock(recordMutex);
    found.frame = recordingFrame();
    recordedLists[keyOf(context)].velocity = std::move(found);
}

void recordUiTargetUse(ID3D11DeviceContext* context)
{
    std::lock_guard lock(recordMutex);
    recordedLists[keyOf(context)].drawsOnUiTarget = true;
}

// The first main-view upload after the immediate context executed anything starts a frame.
void startFrame()
{
    frame.index++;
    frame.jittered = jitterNext.load() && replacing.load();
    frame.jitter = frame.jittered ? camera::jitterPixels(frame.index, jitterPhases.load()) : std::array<float, 2>{};
    frame.view.reset();
}

void sceneWritten(camera::SceneConstants& scene)
{
    const auto kind = camera::classify(scene);
    if (kind == camera::SceneView::Other) return;

    std::lock_guard lock(recordMutex);
    if (executedSinceFrameStart)
    {
        executedSinceFrameStart = false;
        startFrame();
    }

    if (kind == camera::SceneView::MainCamera && !frame.view) frame.view = camera::viewOf(scene);
    if (frame.jittered) camera::applyJitter(scene, camera::jitterNdc(frame.jitter, scene.c[22][0], scene.c[22][1]));
}

// ---- Resources ----

ComPtr<ID3DBlob> compile(const char* source, const char* name, const char* profile)
{
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source, std::strlen(source), name, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error(std::string("Shader ") + name + " did not compile: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                      : hex(static_cast<std::uint32_t>(result))));

    return code;
}

ComPtr<ID3D11Buffer> constantBuffer(UINT size)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()), "Creating a DLSS constant buffer");
    return buffer;
}

void ensurePipeline()
{
    if (ownState) return;

    auto code = compile(shaders::Inputs, "dlss-inputs", "cs_5_0");
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, inputShader.GetAddressOf()),
        "Creating the DLSS input shader");
    code = compile(shaders::FullScreen, "dlss-full-screen", "vs_5_0");
    check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, fullScreenShader.GetAddressOf()),
        "Creating the DLSS full-screen shader");
    code = compile(shaders::Composite, "dlss-composite", "ps_5_0");
    check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, compositeShader.GetAddressOf()),
        "Creating the DLSS composite shader");
    code = compile(shaders::StretchVertex, "dlss-stretch-vertex", "vs_5_0");
    check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, stretchVertexShader.GetAddressOf()),
        "Creating the stretch vertex shader");
    code = compile(shaders::Stretch, "dlss-stretch", "ps_5_0");
    check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, stretchShader.GetAddressOf()),
        "Creating the stretch shader");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&sampler, linearSampler.GetAddressOf()), "Creating the stretch sampler");
    inputConstants = constantBuffer(sizeof(InputConstants));
    compositeConstants = constantBuffer(sizeof(CompositeConstants));

    // A separate context state lets DLSS run between the game's command lists without touching the
    // state the immediate context carries.
    ComPtr<ID3D11Device1> device1;
    check(device.As(&device1), "Finding Direct3D 11.1");
    const auto level = device->GetFeatureLevel();
    const UINT flags =
        (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    check(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, ownState.GetAddressOf()),
        "Creating the DLSS device context state");
}

void ensureTarget(Target& target, UINT width, UINT height, DXGI_FORMAT format, const char* name)
{
    if (target.texture && target.width == width && target.height == height) return;

    target = {};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    check(device->CreateTexture2D(&desc, nullptr, target.texture.GetAddressOf()), name);
    check(device->CreateShaderResourceView(target.texture.Get(), nullptr, target.view.GetAddressOf()), name);
    check(device->CreateUnorderedAccessView(target.texture.Get(), nullptr, target.access.GetAddressOf()), name);
    target.width = width;
    target.height = height;
}

// The colour DLSS receives is already tone-mapped, with white at 1, so the exposure it is told is a
// constant 1. Handing that over as a texture, rather than leaving it out, keeps add-ons that read the
// evaluation (OptiScaler) from turning on DLSS's auto-exposure.
void ensureExposure()
{
    if (exposureTexture) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    constexpr float exposure = 1.0f;
    const D3D11_SUBRESOURCE_DATA data{&exposure, sizeof(exposure), sizeof(exposure)};
    check(device->CreateTexture2D(&desc, &data, exposureTexture.GetAddressOf()), "Creating the DLSS exposure texture");
}

// The depth copy the forward passes add to is only ever a depth target in the game, so it gets its own
// shader view. False when the game made it without one.
bool ensureDepthView(ID3D11DepthStencilView* depth)
{
    const auto resource = resourceOf(depth);
    if (resource == depthResource) return depthView != nullptr;

    depthResource = resource;
    depthView.Reset();
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    if (FAILED(resource.As(&texture))) return false;

    texture->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;

    return SUCCEEDED(device->CreateShaderResourceView(texture.Get(), &viewDesc, depthView.GetAddressOf()));
}

// ---- NGX ----

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::string text(message ? message : "");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    log("NGX: " + text);
}

void releaseFeature()
{
    if (ngx.feature) NVSDK_NGX_D3D11_ReleaseFeature(ngx.feature);
    ngx.feature = nullptr;
    ngx.key = {};
}

void releaseNgx()
{
    releaseFeature();
    if (ngx.parameters) NVSDK_NGX_D3D11_DestroyParameters(ngx.parameters);
    ngx.parameters = nullptr;
    if (ngx.initialized && ngxDevice) NVSDK_NGX_D3D11_Shutdown1(ngxDevice.Get());
    ngx.initialized = false;
}

// Throws with a message for the player when DLSS cannot run on this PC.
void initializeNgx()
{
    if (ngx.initialized) return;

    // nvngx_dlss.dll sits beside the add-on, in the data directory.
    const auto modelDirectory = dataDirectory.wstring();
    const wchar_t* paths[]{modelDirectory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    auto result = NVSDK_NGX_D3D11_Init_with_ProjectID(NgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Fox Engine 1.0.15", dataDirectory.c_str(),
        ngxDevice.Get(), &info);
    if (NVSDK_NGX_FAILED(result))
        throw std::runtime_error("NVIDIA NGX could not start (" + hex(static_cast<std::uint32_t>(result))
            + "). DLSS needs an NVIDIA RTX GPU and a current driver.");

    ngx.initialized = true;
    int available{};
    result = NVSDK_NGX_D3D11_GetCapabilityParameters(&ngx.parameters);
    if (NVSDK_NGX_SUCCEED(result)) NVSDK_NGX_Parameter_GetI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available)
    {
        int needsDriver{};
        unsigned major{}, minor{};
        if (ngx.parameters)
        {
            NVSDK_NGX_Parameter_GetI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            NVSDK_NGX_Parameter_GetUI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
            NVSDK_NGX_Parameter_GetUI(ngx.parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        }

        releaseNgx();
        throw std::runtime_error(needsDriver ? "DLSS needs NVIDIA driver " + std::to_string(major) + "." + std::to_string(minor) + " or newer."
                                             : std::string("DLSS is unavailable on this GPU. nvngx_dlss.dll must be beside the add-on."));
    }

    log("NGX initialized; DLSS is available. Model directory: " + dataDirectory.string());
}

NVSDK_NGX_PerfQuality_Value perfQuality(Mode mode, float scale)
{
    switch (mode)
    {
    case Mode::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case Mode::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case Mode::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case Mode::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    case Mode::Custom:
        return scale >= 0.99f ? NVSDK_NGX_PerfQuality_Value_DLAA : scale >= 0.66f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
            : scale >= 0.58f ? NVSDK_NGX_PerfQuality_Value_Balanced : scale >= 0.5f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
            : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default: return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
}

// DLSS's own optimal render size for the preset modes, the nominal scale for Custom. Odd sizes are
// rounded up to even, as the engine rounds its own (0x2496A0).
Plan makePlan(const Settings& wanted, UINT outputWidth, UINT outputHeight)
{
    const auto scale = nominalScale(wanted.mode, wanted.customScale);
    Plan result{0, 0, outputWidth, outputHeight, perfQuality(wanted.mode, scale)};
    UINT maxWidth{}, maxHeight{}, minWidth{}, minHeight{};
    float sharpness{};
    if (wanted.mode == Mode::Custom
        || NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(ngx.parameters, outputWidth, outputHeight, result.quality, &result.renderWidth,
            &result.renderHeight, &maxWidth, &maxHeight, &minWidth, &minHeight, &sharpness))
        || !result.renderWidth || !result.renderHeight)
    {
        result.renderWidth = static_cast<UINT>(std::lround(outputWidth * scale));
        result.renderHeight = static_cast<UINT>(std::lround(outputHeight * scale));
    }

    result.renderWidth = std::min(result.renderWidth + (result.renderWidth & 1), outputWidth);
    result.renderHeight = std::min(result.renderHeight + (result.renderHeight & 1), outputHeight);

    return result;
}

// Returns true when the feature was created, which starts a new history.
bool ensureFeature(const Plan& target, const Settings& wanted)
{
    const FeatureKey key{target, wanted.preset};
    if (ngx.feature && ngx.key == key) return false;

    releaseFeature();
    for (const char* hint : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(ngx.parameters, hint, static_cast<unsigned>(wanted.preset));

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = target.renderWidth;
    create.Feature.InHeight = target.renderHeight;
    create.Feature.InTargetWidth = target.outputWidth;
    create.Feature.InTargetHeight = target.outputHeight;
    create.Feature.InPerfQualityValue = target.quality;
    // The motion vectors are per render pixel; reversed-Z depth; the colour is LDR.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    const auto result = NGX_D3D11_CREATE_DLSS_EXT(ngxContext.Get(), &ngx.feature, ngx.parameters, &create);
    if (NVSDK_NGX_FAILED(result))
    {
        ngx.feature = nullptr;
        throw std::runtime_error("Creating the DLSS feature failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
    }

    ngx.key = key;
    log("Created DLSS " + std::string(label(wanted.mode)) + ": " + std::to_string(target.renderWidth) + "x" + std::to_string(target.renderHeight)
        + " -> " + std::to_string(target.outputWidth) + "x" + std::to_string(target.outputHeight) + ", preset "
        + std::string(label(wanted.preset)) + ".");

    return true;
}

// The SDK's NGX_D3D11_EVALUATE_DLSS_EXT ends in NVSDK_NGX_D3D11_EvaluateFeature_C. Add-ons that
// follow a game's DLSS (RenoDX hooks the non-_C entry point) only see evaluations made through
// NVSDK_NGX_D3D11_EvaluateFeature, so set the same parameters and call that.
NVSDK_NGX_Result evaluateDlss(const NVSDK_NGX_D3D11_DLSS_Eval_Params& evaluation)
{
    auto* parameters = ngx.parameters;
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_Color, evaluation.Feature.pInColor);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_Output, evaluation.Feature.pInOutput);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_Depth, evaluation.pInDepth);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_MotionVectors, evaluation.pInMotionVectors);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_TransparencyMask, nullptr);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_ExposureTexture, evaluation.pInExposureTexture);
    NVSDK_NGX_Parameter_SetD3d11Resource(parameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, nullptr);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, evaluation.InJitterOffsetX);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, evaluation.InJitterOffsetY);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Sharpness, 0.0f);
    NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_Reset, evaluation.InReset);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_MV_Scale_X, evaluation.InMVScaleX);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_MV_Scale_Y, evaluation.InMVScaleY);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, evaluation.InFrameTimeDeltaInMsec);
    for (const char* base : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
             NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
             NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
             NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y})
        NVSDK_NGX_Parameter_SetUI(parameters, base, 0);
    NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, evaluation.InRenderSubrectDimensions.Width);
    NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, evaluation.InRenderSubrectDimensions.Height);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);

    return NVSDK_NGX_D3D11_EvaluateFeature(ngxContext.Get(), ngx.feature, parameters, nullptr);
}

// ---- Per frame, on the immediate context ----

// Runs the replacement's Direct3D work in its own context state, so the game's is untouched.
template<class Work> void inOwnState(Work&& work)
{
    Bypass guard;
    ComPtr<ID3DDeviceContextState> gameState;
    context1->SwapDeviceContextState(ownState.Get(), gameState.GetAddressOf());

    const auto restore = [&]
    {
        immediate->ClearState();
        context1->SwapDeviceContextState(gameState.Get(), nullptr);
    };

    try
    {
        work();
    }
    catch (...)
    {
        restore();
        throw;
    }

    restore();
}

void runInputs(const FxaaPass& pass, const Frame& current, const camera::Matrix& previousFromCurrent, ID3D11ShaderResourceView* objectVelocity)
{
    InputConstants constants{};
    for (int i = 0; i < 16; ++i) constants.previousFromCurrent[i / 4][i % 4] = static_cast<float>(previousFromCurrent[i]);
    const auto ndc = camera::jitterNdc(current.jitter, static_cast<float>(colorTarget.width), static_cast<float>(colorTarget.height));
    constants.jitterNdc[0] = ndc[0];
    constants.jitterNdc[1] = ndc[1];
    constants.renderSize[0] = colorTarget.width;
    constants.renderSize[1] = colorTarget.height;
    constants.objectMotion = objectVelocity != nullptr;
    immediate->UpdateSubresource(inputConstants.Get(), 0, nullptr, &constants, 0, 0);

    ID3D11ShaderResourceView* inputs[]{pass.sceneCopy.Get(), depthView.Get(), objectVelocity};
    ID3D11UnorderedAccessView* outputs[]{colorTarget.access.Get(), depthTarget.access.Get(), motionTarget.access.Get()};
    immediate->CSSetShader(inputShader.Get(), nullptr, 0);
    immediate->CSSetConstantBuffers(0, 1, inputConstants.GetAddressOf());
    immediate->CSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
    immediate->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(outputs)), outputs, nullptr);
    immediate->Dispatch((colorTarget.width + 7) / 8, (colorTarget.height + 7) / 8, 1);
    immediate->ClearState();
}

void runComposite(ID3D11RenderTargetView* destination, ID3D11ShaderResourceView* objectVelocity, const Diagnostics& options, float sharpness)
{
    const CompositeConstants constants{static_cast<std::uint32_t>(options.debugView), sharpness, objectVelocity != nullptr, 0};
    immediate->UpdateSubresource(compositeConstants.Get(), 0, nullptr, &constants, 0, 0);

    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(outputTarget.width), static_cast<float>(outputTarget.height), 0, 1};
    ID3D11ShaderResourceView* inputs[]{outputTarget.view.Get(), motionTarget.view.Get(), objectVelocity};
    immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    immediate->VSSetShader(fullScreenShader.Get(), nullptr, 0);
    immediate->PSSetShader(compositeShader.Get(), nullptr, 0);
    immediate->PSSetConstantBuffers(0, 1, compositeConstants.GetAddressOf());
    immediate->PSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
    immediate->OMSetRenderTargets(1, &destination, nullptr);
    immediate->RSSetViewports(1, &viewport);
    immediate->Draw(3, 0);
}

// Without a DLSS image this frame, the UI target gets the engine's scene stretched, as GammaControl
// would have shown it, so the UI never draws over a stale frame.
void stretchToUiTarget(const ui_redirect::Output& output)
{
    inOwnState([&]
    {
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(output.width), static_cast<float>(output.height), 0, 1};
        immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        immediate->VSSetShader(stretchVertexShader.Get(), nullptr, 0);
        immediate->PSSetShader(stretchShader.Get(), nullptr, 0);
        immediate->PSSetShaderResources(0, 1, output.scene.GetAddressOf());
        immediate->PSSetSamplers(0, 1, linearSampler.GetAddressOf());
        immediate->OMSetRenderTargets(1, output.target.GetAddressOf(), nullptr);
        immediate->RSSetViewports(1, &viewport);
        immediate->Draw(3, 0);
    });
}

bool replaceFxaa(const FxaaPass& pass)
{
    Frame current;
    {
        std::lock_guard lock(recordMutex);
        current = frame;
    }

    Settings wanted;
    Diagnostics options;
    {
        std::lock_guard lock(settingsMutex);
        wanted = settings;
        options = diagnostics;
    }

    // Each frame is recorded before it executes; a later frame's uploads would have started a new one.
    if (pass.frame != current.index) return fallback("the frame's command lists were not recorded in step with their execution");
    if (lastEvaluated && lastEvaluated->index == current.index) return fallback("the frame has a second FXAA pass");
    if (!current.view) return fallback("the frame has no main camera");
    if (!pass.sceneCopy || !pass.sceneTarget || !pass.depth) return fallback("the FXAA pass has an unexpected binding");

    const auto source = textureDesc(pass.sceneCopy.Get()), target = textureDesc(pass.sceneTarget.Get()), depth = textureDesc(pass.depth.Get());
    const UINT width = source.Width, height = source.Height;
    if (!width || target.Width != width || target.Height != height || depth.Width != width || depth.Height != height)
        return fallback("the FXAA pass's textures differ in size");
    if (current.view->width != static_cast<float>(width) || current.view->height != static_cast<float>(height))
        return fallback("the camera's viewport is not the FXAA pass's size");
    if (!ensureDepthView(pass.depth.Get())) return fallback("the depth copy cannot be read");

    // The upscaling modes run at the planned size, DLAA at the engine's own; after a mode change the
    // engine reaches either once it has rebuilt its targets.
    Plan planned{width, height, width, height, NVSDK_NGX_PerfQuality_Value_DLAA};
    if (upscales(wanted.mode))
    {
        if (!plan) return fallback("no DLSS plan yet");
        if (width != plan->renderWidth || height != plan->renderHeight) return fallback("waiting for the requested render size");

        planned = *plan;
    }
    else if (width < outputSize[0] && height < outputSize[1])
        return fallback("waiting for the requested render size");

    // Upscaled frames go to the UI target, which the UI then draws on; DLAA writes where FXAA wrote.
    const bool upscaled = planned.renderWidth != planned.outputWidth || planned.renderHeight != planned.outputHeight;
    const auto uiTarget = upscaled ? ui_redirect::output() : ui_redirect::Output{};
    if (upscaled && (uiTarget.width != planned.outputWidth || uiTarget.height != planned.outputHeight)) return fallback("the UI target is not ready");

    ensurePipeline();
    initializeNgx();
    ensureExposure();
    ensureTarget(colorTarget, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the DLSS colour texture");
    ensureTarget(depthTarget, width, height, DXGI_FORMAT_R32_FLOAT, "Creating the DLSS depth texture");
    ensureTarget(motionTarget, width, height, DXGI_FORMAT_R16G16_FLOAT, "Creating the DLSS motion texture");
    ensureTarget(outputTarget, planned.outputWidth, planned.outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the DLSS output texture");

    // A gap means DLSS missed frames; its history and the previous camera are stale.
    bool reset = !lastEvaluated || lastEvaluated->index + 1 != current.index;
    if (!reset && camera::cameraCut(*lastEvaluated->view, *current.view))
    {
        reset = true;
        ++cameraCuts;
    }

    const auto& previous = reset ? *current.view : *lastEvaluated->view;
    const auto reprojection = camera::previousFromCurrent(previous, *current.view);
    auto* objectVelocity = velocity && velocity->frame == current.index ? velocity->view.Get() : nullptr;
    if (objectVelocity && textureDesc(objectVelocity).Width != width) objectVelocity = nullptr;

    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const auto frameTime = lastEvaluation.QuadPart
        ? static_cast<float>(static_cast<double>(now.QuadPart - lastEvaluation.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart))
        : 16.7f;

    inOwnState([&]
    {
        runInputs(pass, current, reprojection, objectVelocity);
        reset = ensureFeature(planned, wanted) || reset;

        NVSDK_NGX_D3D11_DLSS_Eval_Params evaluation{};
        evaluation.Feature.pInColor = colorTarget.texture.Get();
        evaluation.Feature.pInOutput = outputTarget.texture.Get();
        evaluation.pInDepth = depthTarget.texture.Get();
        evaluation.pInMotionVectors = motionTarget.texture.Get();
        evaluation.pInExposureTexture = exposureTexture.Get();
        evaluation.InJitterOffsetX = current.jitter[0];
        evaluation.InJitterOffsetY = current.jitter[1];
        evaluation.InRenderSubrectDimensions = {width, height};
        evaluation.InReset = reset ? 1 : 0;
        evaluation.InMVScaleX = 1;
        evaluation.InMVScaleY = 1;
        evaluation.InFrameTimeDeltaInMsec = frameTime;
        const auto result = evaluateDlss(evaluation);
        immediate->ClearState();
        if (NVSDK_NGX_FAILED(result)) throw std::runtime_error("DLSS evaluation failed (" + hex(static_cast<std::uint32_t>(result)) + ").");

        runComposite(upscaled ? uiTarget.target.Get() : pass.sceneTarget.Get(), objectVelocity, options, wanted.sharpness);
    });

    if (upscaled) uiTargetFilled = true;
    if (reset) ++resets;
    if (++evaluations == 1)
        log("First DLSS frame: " + std::to_string(width) + "x" + std::to_string(height) + " -> " + std::to_string(planned.outputWidth) + "x"
            + std::to_string(planned.outputHeight) + (objectVelocity ? ", with" : ", without") + " the engine's object velocity.");

    lastEvaluated = current;
    lastEvaluation = now;
    lastEvaluatedPresent = presents;

    return true;
}

// Called at the immediate context's next call after the FXAA pass's command list.
void runPending()
{
    if (!pending) return;

    const auto pass = std::move(*pending);
    pending.reset();

    if (!replacing)
    {
        // Off gives back the feature's memory; the comparison with FXAA keeps it for a quick switch.
        if (currentSettings().mode == Mode::Off) releaseFeature();
        jitterNext = false;
        return;
    }

    bool evaluated{};
    try
    {
        evaluated = replaceFxaa(pass);
    }
    catch (const std::exception& error)
    {
        fail(error.what());
    }

    jitterNext = evaluated && !currentDiagnostics().noJitter;
}

// Before a command list that draws on the UI target runs. DLSS has written it this frame, or the
// engine's scene is stretched into it.
void fillUiTarget()
{
    if (uiTargetFilled) return;

    const auto output = ui_redirect::output();
    if (!output.target || !output.scene)
    {
        logOnce("ui-target", "The UI target is not ready; a frame shows only the UI.");
        return;
    }

    ensurePipeline();
    stretchToUiTarget(output);
    uiTargetFilled = true;
}

// Chooses the render size for the selected mode and asks the engine for it; Off and DLAA leave the
// engine its own. On the immediate context's thread, once per frame.
void updatePlan()
{
    const auto wanted = currentSettings();
    if (!supported || failed || !upscales(wanted.mode) || !render_scale::hooked() || !outputSize[0] || !outputSize[1])
    {
        plan.reset();
        jitterPhases = camera::jitterPhases(1.0f);
        targetMipBias = replacing ? mipBias(1.0f, wanted.mipBiasOffset) : 0.0f;
        render_scale::request({}, {});
        return;
    }

    // Only the mode, the custom scale and the output size decide the render size: anything else
    // would rebuild the engine's targets for nothing.
    if (!plan || planSettings.mode != wanted.mode || planSettings.customScale != wanted.customScale || plan->outputWidth != outputSize[0]
        || plan->outputHeight != outputSize[1])
    {
        initializeNgx();
        plan = makePlan(wanted, outputSize[0], outputSize[1]);
        planSettings = wanted;
        log("DLSS " + std::string(label(wanted.mode)) + " renders at " + std::to_string(plan->renderWidth) + "x" + std::to_string(plan->renderHeight)
            + " for " + std::to_string(plan->outputWidth) + "x" + std::to_string(plan->outputHeight) + ".");
    }

    const float scale = static_cast<float>(plan->renderWidth) / static_cast<float>(plan->outputWidth);
    jitterPhases = camera::jitterPhases(scale);
    targetMipBias = mipBias(scale, wanted.mipBiasOffset);
    render_scale::request({plan->renderWidth, plan->renderHeight}, {plan->outputWidth, plan->outputHeight});
}

// Fox Engine's material samplers repeat their textures; the screen-space passes sample with point or
// clamped samplers, and a bias would also shift their explicit mip reads.
bool materialSampler(const D3D11_SAMPLER_DESC& sampler)
{
    const auto tiles = [](D3D11_TEXTURE_ADDRESS_MODE mode) { return mode == D3D11_TEXTURE_ADDRESS_WRAP || mode == D3D11_TEXTURE_ADDRESS_MIRROR; };
    return sampler.MaxLOD > sampler.MinLOD && tiles(sampler.AddressU) && tiles(sampler.AddressV) && sampler.Filter != D3D11_FILTER_MIN_MAG_MIP_POINT
        && !D3D11_DECODE_IS_COMPARISON_FILTER(sampler.Filter);
}

// The biased copy of a material sampler, made once per sampler and bias; null for other samplers.
// Copies are kept, with the game's sampler, until detach(), so a bound one is never released.
ID3D11SamplerState* biasedCopy(ID3D11SamplerState* sampler, float bias)
{
    const std::pair key{sampler, bias};
    {
        std::shared_lock lock(samplerMutex);
        const auto found = biasedSamplers.find(key);
        if (found != biasedSamplers.end()) return found->second.biased.Get();
    }

    BiasedSampler entry{sampler, nullptr};
    D3D11_SAMPLER_DESC desc{};
    sampler->GetDesc(&desc);
    if (materialSampler(desc) && device)
    {
        desc.MipLODBias = std::clamp(desc.MipLODBias + bias, D3D11_MIP_LOD_BIAS_MIN, D3D11_MIP_LOD_BIAS_MAX);
        device->CreateSamplerState(&desc, entry.biased.GetAddressOf());
        log("Texture LOD bias " + std::to_string(bias) + " on a material sampler (filter " + hex(desc.Filter) + ", anisotropy "
            + std::to_string(desc.MaxAnisotropy) + ").");
    }

    std::unique_lock lock(samplerMutex);

    return biasedSamplers.try_emplace(key, std::move(entry)).first->second.biased.Get();
}

void savePendingSettings(std::uint64_t settle)
{
    Settings unsaved;
    {
        std::lock_guard lock(settingsMutex);
        if (!unsavedChange || GetTickCount64() - *unsavedChange < settle) return;

        unsaved = settings;
        unsavedChange.reset();
    }

    try
    {
        saveSettings(settingsPath, unsaved);
    }
    catch (const std::exception& error)
    {
        log(std::string("Could not save the DLSS settings: ") + error.what());
    }
}
}

void initialize(const std::filesystem::path& directory, bool supportedBuild) noexcept
{
    dataDirectory = directory;
    settingsPath = directory / "MGSVDLSS.ini";
    supported = supportedBuild;

    std::lock_guard lock(settingsMutex);
    try
    {
        settings = loadSettings(settingsPath);
    }
    catch (const std::exception& error)
    {
        log(std::string("Could not load the DLSS settings: ") + error.what());
    }

    updateReplacing();
    frame_rate::setUnlocked(settings.unlockFps);
    log(std::string("DLSS mode ") + std::string(label(settings.mode)) + (supported ? "." : "; this game build is not supported, so it stays off."));
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* context, ID3D11DeviceContext* wrapped) noexcept
{
    std::lock_guard lock(executionMutex);
    device = gameDevice;
    immediate = context;
    ngxContext = wrapped ? wrapped : context;
    ngxContext->GetDevice(ngxDevice.ReleaseAndGetAddressOf());
    if (FAILED(immediate.As(&context1))) fail("Direct3D 11.1 is not available.");
    ui_redirect::attach(gameDevice);
}

void detach() noexcept
{
    savePendingSettings(0);
    std::lock_guard lock(executionMutex);
    replacing = false;
    targetMipBias = 0;
    motion_blur::setVelocityOnly(false);
    fxaa::setForced(false);
    render_scale::request({}, {});
    ui_redirect::detach();
    pending.reset();
    velocity.reset();
    lastEvaluated.reset();
    plan.reset();
    releaseNgx();
    colorTarget = depthTarget = motionTarget = outputTarget = {};
    exposureTexture.Reset();
    depthResource.Reset();
    depthView.Reset();
    inputShader.Reset();
    fullScreenShader.Reset();
    compositeShader.Reset();
    stretchVertexShader.Reset();
    stretchShader.Reset();
    linearSampler.Reset();
    inputConstants.Reset();
    compositeConstants.Reset();
    ownState.Reset();
    context1.Reset();
    ngxContext.Reset();
    ngxDevice.Reset();
    immediate.Reset();
    device.Reset();
    {
        std::lock_guard recordLock(recordMutex);
        recordedLists.clear();
    }

    {
        std::unique_lock samplerLock(samplerMutex);
        biasedSamplers.clear();
    }

    std::lock_guard shaderLock(shaderMutex);
    for (std::size_t pass = 0; pass < std::size(Shaders); ++pass)
        for (std::size_t i = 0; i < CopiesPerShader; ++i)
        {
            gameShaders[pass][i] = nullptr;
            shaderOwners[pass][i].Reset();
        }
}

void setOutputSize(unsigned width, unsigned height) noexcept
{
    {
        std::lock_guard lock(executionMutex);
        outputSize[0] = width;
        outputSize[1] = height;
    }

    ui_redirect::setOutputSize(width, height);
}

void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    const auto text = toHex(*checksum);
    for (std::size_t pass = 0; pass < std::size(Shaders); ++pass)
        if (text == Shaders[pass].checksum)
        {
            try
            {
                remember(pass, ComPtr<IUnknown>(shader));
            }
            catch (...)
            {
            }
        }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (bypass) return false;

    try
    {
        // The UI target is used whenever the engine's targets are smaller than the output, whatever the
        // mode, so the UI and GammaControl always agree on where the frame is.
        if (ui_redirect::drawing2D(context) && ui_redirect::draw2D(context, call))
        {
            recordUiTargetUse(context);

            return true;
        }

        if (call.indexed || call.count != 3 || call.instances != 1) return false;

        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        const auto pass = passOf(shader.Get());
        if (pass == Pass::GammaControl && ui_redirect::drawGamma(context, call))
        {
            recordUiTargetUse(context);

            return true;
        }

        if (!replacing.load(std::memory_order_relaxed)) return false;

        if (pass == Pass::Fxaa) recordFxaa(context);
        else if (pass == Pass::CameraVelocity) recordVelocity(context);
    }
    catch (...)
    {
    }

    return false;
}

bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept
{
    if (bypass || !ui_redirect::clearTarget(context, view, color)) return false;

    try
    {
        recordUiTargetUse(context);
    }
    catch (...)
    {
    }

    return true;
}

void depthStencilCleared(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, float depth, UINT8 stencil) noexcept
{
    if (!bypass) ui_redirect::depthStencilCleared(context, view, flags, depth, stencil);
}

bool biasSamplers(ID3D11DeviceContext* context, unsigned count, ID3D11SamplerState* const* samplers, ID3D11SamplerState** biased) noexcept
{
    // The 2D pass draws at the output size.
    const auto bias = targetMipBias.load(std::memory_order_relaxed);
    if (bypass || bias == 0 || !samplers || count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || ui_redirect::drawing2D(context)) return false;

    try
    {
        bool replaced{};
        for (unsigned i = 0; i < count; ++i)
        {
            biased[i] = samplers[i];
            auto* copy = samplers[i] ? biasedCopy(samplers[i], bias) : nullptr;
            if (!copy) continue;

            biased[i] = copy;
            replaced = true;
        }

        return replaced;
    }
    catch (...)
    {
        return false;
    }
}

void bufferMapped(ID3D11Resource* buffer, void* data) noexcept
{
    if (bypass || !data) return;

    D3D11_RESOURCE_DIMENSION dimension{};
    buffer->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return;

    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(buffer)->GetDesc(&desc);
    if (desc.ByteWidth != SceneConstantsSize || !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;

    mappedScene = buffer;
    mappedSceneData = data;
}

void bufferUnmapped(ID3D11Resource* buffer) noexcept
{
    if (!mappedScene || buffer != mappedScene) return;

    auto* data = mappedSceneData;
    mappedScene = nullptr;
    mappedSceneData = nullptr;

    try
    {
        sceneWritten(*static_cast<camera::SceneConstants*>(data));
    }
    catch (...)
    {
    }
}

void recorded(std::uint64_t into, std::uint64_t from) noexcept
{
    try
    {
        std::lock_guard lock(recordMutex);
        const auto found = recordedLists.find(from);
        if (found == recordedLists.end()) return;

        auto entry = std::move(found->second);
        recordedLists.erase(found);
        auto& target = recordedLists[into];
        if (entry.fxaa) target.fxaa = std::move(entry.fxaa);
        if (entry.velocity) target.velocity = std::move(entry.velocity);
        target.drawsOnUiTarget = target.drawsOnUiTarget || entry.drawsOnUiTarget;
    }
    catch (...)
    {
    }
}

void forget(std::uint64_t list) noexcept
{
    std::lock_guard lock(recordMutex);
    recordedLists.erase(list);
}

void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept
{
    if (bypass) return;

    try
    {
        std::lock_guard lock(executionMutex);
        if (context != immediate.Get()) return;

        runPending();
        Recorded entry;
        {
            std::lock_guard recordLock(recordMutex);
            executedSinceFrameStart = true;
            const auto found = recordedLists.find(list);
            if (found == recordedLists.end()) return;

            entry = std::move(found->second);
            recordedLists.erase(found);
        }

        if (entry.velocity) velocity = std::move(entry.velocity);
        if (entry.fxaa) pending = std::move(entry.fxaa);
        if (!entry.drawsOnUiTarget) return;

        // DLSS runs after a list; within one it would cover the UI drawn after FXAA.
        if (pending && entry.fxaa)
        {
            pending.reset();
            fallback("the FXAA pass and the UI share a command list");
        }

        fillUiTarget();
    }
    catch (const std::exception& error)
    {
        fail(error.what());
    }
}

void present() noexcept
{
    savePendingSettings(500);
    std::lock_guard lock(executionMutex);
    // The game executes its UI and output after FXAA; without them DLSS would have had no place to run.
    if (pending)
    {
        pending.reset();
        fallback("no command list followed the FXAA pass");
    }

    // A frame DLSS did not run on (menus, loading screens, a fallback) leaves the next one unjittered,
    // so a 3D view without FXAA never shakes.
    if (lastEvaluatedPresent != presents) jitterNext = false;
    ++presents;
    uiTargetFilled = false;

    try
    {
        updatePlan();
    }
    catch (const std::exception& error)
    {
        fail(error.what());
    }

    if (replacing && presents % 1800 == 0)
        log("Status: " + std::to_string(evaluations) + " DLSS frames, " + std::to_string(fallbacks) + " without, "
            + std::to_string(resets) + " history resets (" + std::to_string(cameraCuts) + " at camera cuts); last fallback: "
            + (lastFallback.empty() ? "none" : lastFallback) + ".");
}

Status status()
{
    Status result;
    {
        std::lock_guard lock(shaderMutex);
        result.fxaaRecognised = gameShaders[static_cast<std::size_t>(Pass::Fxaa)][0].load() != nullptr;
    }

    std::lock_guard lock(executionMutex);
    result.supportedBuild = supported;
    result.failed = failed;
    result.failure = failure;
    result.lastFallback = lastFallback;
    result.running = evaluations && lastEvaluatedPresent + 2 >= presents;
    result.upscalingAvailable = render_scale::hooked();
    result.renderWidth = colorTarget.width;
    result.renderHeight = colorTarget.height;
    result.outputWidth = outputTarget.width;
    result.outputHeight = outputTarget.height;
    result.mipBias = targetMipBias;
    result.evaluations = evaluations;
    result.fallbacks = fallbacks;
    result.resets = resets;

    if (lastEvaluated)
    {
        result.jitter[0] = lastEvaluated->jitter[0];
        result.jitter[1] = lastEvaluated->jitter[1];
    }

    return result;
}

Settings currentSettings()
{
    std::lock_guard lock(settingsMutex);
    return settings;
}

void applySettings(const Settings& wanted)
{
    {
        std::lock_guard lock(executionMutex);
        // Choosing a mode again retries after a failure.
        if (wanted.mode != currentSettings().mode) failed = false;
    }

    std::lock_guard lock(settingsMutex);
    settings = wanted;
    unsavedChange = GetTickCount64();
    updateReplacing();
    frame_rate::setUnlocked(settings.unlockFps);
}

Diagnostics currentDiagnostics()
{
    std::lock_guard lock(settingsMutex);
    return diagnostics;
}

void applyDiagnostics(const Diagnostics& options)
{
    std::lock_guard lock(settingsMutex);
    diagnostics = options;
    updateReplacing();
}
}
