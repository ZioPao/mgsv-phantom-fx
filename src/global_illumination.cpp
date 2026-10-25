// Ray-traced global illumination; global_illumination.hpp describes the route.
#include "global_illumination.hpp"

#include "camera.hpp"
#include "engine_scene.hpp"
#include "gi_inputs.hpp"
#include "gi_lighting.hpp"
#include "gi_renderer.hpp"
#include "gi_textures.hpp"
#include "indirect_light.hpp"
#include "local_lights.hpp"
#include "log.hpp"
#include "performance.hpp"
#include "shader_bytecode.hpp"
#include "shaders/gi_shared.h"

#include <Windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace phantom_fx::global_illumination
{
namespace
{
using Microsoft::WRL::ComPtr;

// SH_SphereMap, which lights the probe atlas with the sky's light (docs/DEVELOPMENT.md, "Global
// illumination", "Light").
constexpr char SkyProbeChecksum[] = "4a3faf87b9ec54a401060a5b736eae5a";
// MotionBlurCameraVelocity, which reads the game's velocity for its moving meshes at t3, right after
// the G-buffer (docs/DEVELOPMENT.md, "Velocity").
constexpr char CameraVelocityChecksum[] = "5d0241dd9804f41c780cd36669c71198";
constexpr UINT ObjectVelocitySlot = 3;
constexpr UINT SceneSlot = 2, LightSlot = 3, ProbeSlot = 7;

// Packs what the kernels read from the G-buffer: the depth, the normals in world space (0 where
// the sky is; alpha 1/3 on foliage, the specular target's material 43, else 1) and the albedo, linear.
constexpr char PackSource[] = R"(
Texture2D<float> g_Depth : register(t0);
Texture2D<float4> g_Normal : register(t1);
Texture2D<float4> g_Albedo : register(t2);
Texture2D<float4> g_Specular : register(t3);
RWTexture2D<float> g_DepthOut : register(u0);
RWTexture2D<float4> g_NormalOut : register(u1);
RWTexture2D<float4> g_AlbedoOut : register(u2);

cbuffer Parameters : register(b0)
{
    float4 g_View[3]; // World to view, rows
    uint2 g_Size;
    uint g_HasAlbedo;
    uint g_HasSpecular;
};

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= g_Size)) return;

    float depth = g_Depth.Load(int3(id, 0));
    g_DepthOut[id] = depth;

    // DecodeViewSpaceNormal: z = 2 b^2 - 1, xy the direction of 2 rg - 1 at length sqrt(1 - z^2).
    float3 encoded = g_Normal.Load(int3(id, 0)).xyz;
    float2 xy = encoded.xy * 2 - 1;
    float z = encoded.z * encoded.z * 2 - 1;
    float rest = 1 - z * z;
    float3 view = float3(rsqrt(dot(xy, xy) * rest + 1e-7) * (rest * xy), z);
    float3 world = normalize(view.x * g_View[0].xyz + view.y * g_View[1].xyz + view.z * g_View[2].xyz);
    bool foliage = g_HasSpecular && round(g_Specular.Load(int3(id, 0)).z * 255) == 43;
    g_NormalOut[id] = depth > 0 ? float4(0.5 * world + 0.5, foliage ? 1.0 / 3 : 1) : 0;

    g_AlbedoOut[id] = float4(g_HasAlbedo ? g_Albedo.Load(int3(id, 0)).rgb : 0.25, 1);
}
)";

struct PackParameters
{
    float view[3][4];
    std::uint32_t width, height, hasAlbedo, hasSpecular;
};

// The light the global illumination keeps in its caches is the game's before exposure, scaled by
// this: the game's auto-exposure changes quickly, and caches holding exposed light lagged behind it
// and overshot (a white screen). The output takes the frame's exposure (gi_renderer::Frame::outputScale).
constexpr float LightScale = 1.0f / 1024;

// The sky's light as PsWork holds it, before exposure, and the exposure the pass had; whether the
// probe's light stands in for it (the game gave no sky).
struct Sky
{
    std::array<std::array<float, 16>, 3> matrices{};
    float exposure{};
    bool fromProbe{};
};

// The sun's colour before exposure (cPSLight c1), and the exposure the sun pass had.
struct Sun
{
    std::array<float, 3> direction{}, color{};
    float exposure{};
};

// What a deferred context, then its command list, recorded that the kick and the wait need.
struct Notes
{
    bool kick{}, wait{};
    ComPtr<ID3D11ShaderResourceView> depth, normals, albedo, velocity;
    std::optional<Sky> sky;
    std::optional<Sun> sun;
    // The game's shadow map range from the sun pass inside it: cPSLight c8.x, c8.y, c6.x, and 1.
    std::optional<std::array<float, 4>> shadowRange;
};

// A shared texture with the views Direct3D 11 uses.
struct SharedTexture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> access;
    UINT width{}, height{};
};

std::mutex optionsMutex;
Options wanted, current;
// The scene runs (for the light, the shadows or the point and spot lights), the light runs, the sun's
// shadows run, the point and spot lights' shadows run.
std::atomic<bool> enabled{}, lighting{}, shadowing{}, localShadowing{}, failed{};

std::shared_mutex shaderMutex;
std::unordered_set<IUnknown*> skyProbeShaders, cameraVelocityShaders;
std::vector<ComPtr<IUnknown>> shaderOwners;

std::mutex notesMutex;
std::unordered_map<std::uint64_t, Notes> notes; // By deferred context, then by command list

// ---- The immediate context's side, guarded by executionMutex ----
std::mutex executionMutex;
ComPtr<ID3D11Device5> device;
ComPtr<ID3D11DeviceContext4> immediate;
ID3D11DeviceContext* nativeImmediate{};
ComPtr<ID3D11Fence> fence;
HANDLE fenceHandle{};
std::uint64_t fenceValue{}, lightSignalled{}, lightWaited{};
std::uint32_t kicks{};
std::optional<camera::SceneConstants> sceneCamera, previousCamera;
// The game's velocity for its moving meshes, whether a list that drew it ran since the last present and
// before the kick (only then does it hold this frame's), and its shared copy for the shadow denoiser.
ComPtr<ID3D11ShaderResourceView> objectVelocity;
bool objectVelocityDrawn{};
ComPtr<ID3D11Texture2D> objectVelocityCopy;
std::optional<Sky> sky;
std::optional<Sun> sun;
std::array<float, 4> shadowRange{};
ComPtr<ID3D11ShaderResourceView> normals, albedo;
ComPtr<ID3D11ComputeShader> packShader;
ComPtr<ID3D11Buffer> packParameters;
// Shared copies of the game's terrain heightmap (gi_renderer::Terrain).
ComPtr<ID3D11Texture2D> terrainPages, terrainHeights;
SharedTexture depthOut, normalOut, albedoOut;
std::string failure;
std::atomic<std::uint64_t> lastKick{}; // GetTickCount64 of the last frame lit
// Diagnostics: the kicks' CPU time in performance counter ticks, and the kicks, since the last log.
std::atomic<std::int64_t> kickTicks{};
std::atomic<std::uint64_t> kicksSinceLog{};
// And its steps' (the rest is the work after the renderer's); how often the terrain's converter
// constants and heightmap changed, and the converter last used.
enum KickStep { InputsStep, ModelsStep, TexturesStep, TerrainStep, SignalStep, RenderStep, KickSteps };
std::array<std::atomic<std::int64_t>, KickSteps> kickStepTicks{};
std::atomic<std::uint32_t> converterChanges{}, heightmapChanges{};
std::array<std::atomic<float>, 8> lastConverter{};
// Diagnostics: the kicks whose shadow denoiser had the game's velocity for its moving meshes, and not.
std::atomic<std::uint64_t> kicksWithObjectVelocity{}, kicksWithoutObjectVelocity{};

// The light the resolve reads and the shadow the sun pass reads, made at the render size on
// whichever thread asks first. Taken after executionMutex when both are.
std::mutex lightMutex;
ComPtr<ID3D11Device> lightDevice;
SharedTexture output, shadowOutput, shadowDebugOutput, localLightOutput, lightDebugOutput;
bool shadowWritten{}, shadowDebugWritten{}; // Direct3D 12 has traced into shadowOutput, shadowDebugOutput
bool lightDebugWritten{}; // And into lightDebugOutput
bool localLightWritten{};                  // This frame's kick traced the point and spot lights into localLightOutput

void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
}

SharedTexture makeShared(ID3D11Device* creator, UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    SharedTexture made{.width = width, .height = height};
    check(creator->CreateTexture2D(&desc, nullptr, made.texture.GetAddressOf()), what);
    check(creator->CreateShaderResourceView(made.texture.Get(), nullptr, made.view.GetAddressOf()), what);
    check(creator->CreateUnorderedAccessView(made.texture.Get(), nullptr, made.access.GetAddressOf()), what);

    return made;
}

std::pair<UINT, UINT> sizeOf(ID3D11View* view)
{
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    if (view) view->GetResource(resource.GetAddressOf());
    if (resource && SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);

    return {desc.Width, desc.Height};
}

Notes& notesOf(ID3D11DeviceContext* context)
{
    return notes[reinterpret_cast<std::uint64_t>(context)];
}

// Call with executionMutex held.
void fail(const std::string& reason)
{
    failed = true;
    failure = reason;
    log("Global illumination stopped: " + reason);
    gi_inputs::setActive(false);
    engine_scene::setActive(false);
    gi_renderer::stop();
    gi_textures::reset();
}

// Call with executionMutex held.
void ensurePipeline()
{
    if (packShader)
    {
        if (!gi_renderer::started()) gi_renderer::start(device.Get(), fenceHandle);
        return;
    }

    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(PackSource, std::strlen(PackSource), "gi-pack", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error("The G-buffer pack did not compile: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : hex(static_cast<std::uint32_t>(result))));

    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, packShader.GetAddressOf()), "Creating the G-buffer pack");
    D3D11_BUFFER_DESC parameters{};
    parameters.ByteWidth = sizeof(PackParameters);
    parameters.Usage = D3D11_USAGE_DYNAMIC;
    parameters.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    parameters.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device->CreateBuffer(&parameters, nullptr, packParameters.GetAddressOf()), "Creating the G-buffer pack's constants");

    check(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), "Creating the shared fence");
    check(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle), "Sharing the fence");
    gi_renderer::start(device.Get(), fenceHandle);
}

// The G-buffer into the shared textures, on the immediate context; call with executionMutex held.
void pack(ID3D11ShaderResourceView* depth, const camera::SceneConstants& scene)
{
    const auto [width, height] = sizeOf(depth);
    if (depthOut.width != width || depthOut.height != height)
    {
        depthOut = makeShared(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, "Creating the shared depth");
        normalOut = makeShared(device.Get(), width, height, DXGI_FORMAT_R10G10B10A2_UNORM, "Creating the shared normals");
        albedoOut = makeShared(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the shared albedo");
        log("Global illumination at " + std::to_string(width) + "x" + std::to_string(height) + ".");
    }

    const bool hasAlbedo = albedo && sizeOf(albedo.Get()) == std::pair{width, height};
    PackParameters parameters{};
    for (int row = 0; row < 3; ++row) std::memcpy(parameters.view[row], scene.c[8 + row], sizeof(parameters.view[row]));
    parameters.width = width;
    parameters.height = height;
    parameters.hasAlbedo = hasAlbedo;
    const auto specular = indirect_light::specularTarget(width, height);
    parameters.hasSpecular = specular != nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(immediate->Map(packParameters.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Writing the G-buffer pack's constants");
    std::memcpy(mapped.pData, &parameters, sizeof(parameters));
    immediate->Unmap(packParameters.Get(), 0);

    ID3D11ShaderResourceView* inputs[]{depth, normals.Get(), hasAlbedo ? albedo.Get() : nullptr, specular.Get()};
    ID3D11UnorderedAccessView* outputs[]{depthOut.access.Get(), normalOut.access.Get(), albedoOut.access.Get()};
    immediate->CSSetShader(packShader.Get(), nullptr, 0);
    immediate->CSSetConstantBuffers(0, 1, packParameters.GetAddressOf());
    immediate->CSSetShaderResources(0, 4, inputs);
    immediate->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
    immediate->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView* noInputs[4]{};
    ID3D11UnorderedAccessView* noOutputs[3]{};
    immediate->CSSetShaderResources(0, 4, noInputs);
    immediate->CSSetUnorderedAccessViews(0, 3, noOutputs, nullptr);
    immediate->CSSetShader(nullptr, nullptr, 0);
}

// Copies the most detailed level of a view's texture into a shared copy for Direct3D 12, made again
// when its size or format changes; the view's format for Direct3D 12's view. Call with executionMutex held.
bool copyShared(ID3D11ShaderResourceView* view, ComPtr<ID3D11Texture2D>& copy, DXGI_FORMAT& format)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    view->GetDesc(&viewDesc);
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> source;
    view->GetResource(resource.GetAddressOf());
    if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || FAILED(resource.As(&source))) return false;

    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const UINT mip = viewDesc.Texture2D.MostDetailedMip;
    D3D11_TEXTURE2D_DESC made{};
    made.Width = std::max(desc.Width >> mip, 1u);
    made.Height = std::max(desc.Height >> mip, 1u);
    made.MipLevels = made.ArraySize = 1;
    made.Format = desc.Format;
    made.SampleDesc.Count = 1;
    made.Usage = D3D11_USAGE_DEFAULT;
    made.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    made.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    D3D11_TEXTURE2D_DESC have{};
    if (copy) copy->GetDesc(&have);
    if (!copy || have.Width != made.Width || have.Height != made.Height || have.Format != made.Format)
    {
        copy.Reset();
        if (FAILED(device->CreateTexture2D(&made, nullptr, copy.GetAddressOf()))) return false;
    }

    immediate->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, source.Get(), D3D11CalcSubresource(mip, 0, desc.MipLevels), nullptr);
    format = viewDesc.Format;
    return true;
}

// The game's terrain heightmap into its shared copies, and its constants, for the frame. Call with
// executionMutex held.
void copyTerrain(const engine_scene::TerrainInputs& inputs, gi_renderer::Frame& frame)
{
    static std::array<ID3D11ShaderResourceView*, 2> lastViews{};
    if (const std::array views{inputs.pages.Get(), inputs.heights.Get()}; views != lastViews)
    {
        if (lastViews[0]) ++heightmapChanges;
        lastViews = views;
    }

    static std::optional<std::array<float, 8>> previous;
    if (previous && *previous != inputs.converter) ++converterChanges;
    previous = inputs.converter;
    for (std::size_t i = 0; i < 8; ++i) lastConverter[i] = inputs.converter[i];

    gi_renderer::Terrain terrain;
    if (!copyShared(inputs.pages.Get(), terrainPages, terrain.pagesFormat) || !copyShared(inputs.heights.Get(), terrainHeights, terrain.heightsFormat))
        return;

    terrain.pages = terrainPages.Get();
    terrain.heights = terrainHeights.Get();
    std::copy_n(inputs.scaleOffset.begin(), 4, terrain.scale.begin());
    std::copy_n(inputs.scaleOffset.begin() + 4, 4, terrain.offset.begin());
    std::copy_n(inputs.converter.begin(), 4, terrain.uvConverter.begin());
    std::copy_n(inputs.converter.begin() + 4, 4, terrain.texelCorrect.begin());
    frame.terrain = terrain;
}

// The rows of one of cPSScene's matrices from its first register: m_projectionView c0, m_projection c4, m_view c8.
constexpr int ProjectionRows = 4, ViewRows = 8;
// cPSScene's m_cameraCenterOffset: the upscaler's jitter in NDC (camera::applyJitter).
constexpr int JitterRow = 28;

gi_renderer::Matrix rowsOf(const camera::SceneConstants& scene, int first)
{
    gi_renderer::Matrix matrix{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) matrix[row * 4 + column] = scene.c[first + row][column];

    return matrix;
}

gi_renderer::Matrix projectionViewOf(const camera::SceneConstants& scene)
{
    return rowsOf(scene, 0);
}

// Diagnostics: the light the scene gets, every ten seconds.
void logLights(const gi_renderer::Frame& frame, float cameraExposure)
{
    static std::uint64_t lastLightLog{};
    const std::uint64_t now = lastKick;
    if (!frame.traceLight || now - lastLightLog <= 10'000) return;

    lastLightLog = now;
    const auto text = [](const std::array<float, 3>& v) { return std::to_string(v[0]) + " " + std::to_string(v[1]) + " " + std::to_string(v[2]); };
    std::string line = "Global illumination's lights (x 1/1024, before exposure): sun " + text(frame.sunColor) + " towards "
        + text(frame.sunDirection) + "; exposure: main view " + std::to_string(cameraExposure) + ", sun pass "
        + std::to_string(sun ? sun->exposure : 0.0f) + ", probe pass " + std::to_string(sky ? sky->exposure : 0.0f);
    if (sky)
    {
        gi_lighting::SphericalHarmonics radiance{};
        for (int i = 0; i < 9; ++i) radiance[i] = frame.skyRadiance[i];
        // The game's own sky light on an upward surface: c = (0, 0, 1, 1) for n = (0, 1, 0).
        std::array<float, 3> gameUp{};
        for (int color = 0; color < 3; ++color)
        {
            const auto& m = sky->matrices[color];
            gameUp[color] = LightScale * (m[10] + m[11] + m[14] + m[15]);
        }

        line += "; sky radiance up " + text(gi_lighting::evaluate(radiance, {0, 1, 0})) + ", sideways " + text(gi_lighting::evaluate(radiance, {1, 0, 0}))
            + ", down " + text(gi_lighting::evaluate(radiance, {0, -1, 0})) + "; the game's sky light up " + text(gameUp)
            + (sky->fromProbe ? " (no sky from the game: its probe's light)" : "");
    }

    log(line + ".");
}

// Diagnostics: what the scene holds, and the CPU work a frame, every ten seconds while it runs.
void logStatus()
{
    static std::uint64_t lastStatus{};
    const auto now = GetTickCount64();
    if (now - lastStatus <= 10'000) return;

    lastStatus = now;
    log("Global illumination: " + engine_scene::poseTiming() + ".");
    log("Global illumination: " + status() + "; the denoisers had the game's velocity for moving meshes in "
        + std::to_string(kicksWithObjectVelocity.exchange(0)) + " frames, not in " + std::to_string(kicksWithoutObjectVelocity.exchange(0)));

    const auto work = gi_inputs::takeWork();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double frames = static_cast<double>(std::max<std::uint64_t>(kicksSinceLog.exchange(0), 1));
    const auto milliseconds = [&](std::atomic<std::int64_t>& ticks) { return static_cast<double>(ticks.exchange(0)) * 1000.0 / static_cast<double>(frequency.QuadPart); };
    const auto perFrame = [&](double value) { return std::to_string(value / frames); };
    const double kick = milliseconds(kickTicks);
    std::string steps;
    for (const auto [step, name] : {std::pair{InputsStep, "inputs"}, {ModelsStep, "models"}, {TexturesStep, "textures"}, {TerrainStep, "terrain"},
             {SignalStep, "signal"}, {RenderStep, "renderer"}})
        steps += std::string(steps.empty() ? "" : ", ") + name + " " + perFrame(milliseconds(kickStepTicks[step]));
    steps += "; the terrain's converter changed " + std::to_string(converterChanges.exchange(0)) + " times (now";
    for (const auto& value : lastConverter) steps += " " + std::to_string(value.load());
    steps += "), its heightmap textures " + std::to_string(heightmapChanges.exchange(0)) + " times";

    log("Global illumination's CPU time a frame: hooks " + perFrame(work.hookMilliseconds) + " ms (all threads) over " + perFrame(static_cast<double>(work.draws))
        + " mesh draws; the kick " + perFrame(kick) + " ms (render thread: " + steps + ").");
}

// Remakes `texture` at this size unless it has it; whether it did.
bool resizeShared(SharedTexture& texture, ID3D11Device* creator, UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    if (texture.width == width && texture.height == height) return false;

    texture = makeShared(creator, width, height, format, what);
    return true;
}

// Milliseconds since the last frame lit; the most there is before the first.
std::uint64_t sinceKick()
{
    const auto last = lastKick.load(std::memory_order_relaxed);
    return last ? GetTickCount64() - last : UINT64_MAX;
}

// The frame's kick, before the list with the probe upscale runs; call with executionMutex held.
void kick(ID3D11ShaderResourceView* depth, const Options& options)
{
    LARGE_INTEGER stepStarted{};
    QueryPerformanceCounter(&stepStarted);
    const auto endStep = [&](KickStep step) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        kickStepTicks[step] += now.QuadPart - stepStarted.QuadPart;
        stepStarted = now;
    };

    ensurePipeline();
    // Direct3D 12 is done with the shared inputs of the frame before.
    performance::Timed timed(immediate.Get(), performance::Pass::GiWait);
    if (lightSignalled) check(immediate->Wait(fence.Get(), lightSignalled), "Waiting for the global illumination");

    timed.next(performance::Pass::GiInputs);
    if (const auto taken = gi_inputs::takeCamera())
    {
        previousCamera = sceneCamera ? sceneCamera : taken->constants;
        sceneCamera = taken->constants;
    }

    if (!sceneCamera || !depth || !normals) return;

    pack(depth, *sceneCamera);
    ID3D11Texture2D* light{};
    ID3D11Texture2D* shadow{};
    ID3D11Texture2D* shadowDebug{};
    ID3D11Texture2D* lightDebug{};
    {
        std::lock_guard lock(lightMutex);
        resizeShared(output, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the light");
        light = output.texture.Get();
        if (options.enabled && options.debugHistory)
        {
            if (resizeShared(lightDebugOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the light's debug view"))
                lightDebugWritten = false;

            lightDebug = lightDebugOutput.texture.Get();
        }
        if (options.shadows)
        {
            if (resizeShared(shadowOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16_FLOAT, "Creating the shadow")) shadowWritten = false;
            shadow = shadowOutput.texture.Get();
            if (options.debugShadows)
            {
                if (resizeShared(shadowDebugOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R8G8B8A8_UNORM, "Creating the shadow debug view"))
                    shadowDebugWritten = false;

                shadowDebug = shadowDebugOutput.texture.Get();
            }
        }
    }

    // The point and spot lights recorded for this frame, a bit each in their shadow mask.
    std::vector<gi_renderer::LocalLight> localLightsToTrace;
    if (options.localLights) localLightsToTrace = local_lights::takeForTrace(*sceneCamera);
    ID3D11Texture2D* localLightShadow{};
    {
        std::lock_guard lock(lightMutex);
        localLightWritten = false;
        if (!localLightsToTrace.empty())
        {
            resizeShared(localLightOutput, device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16_FLOAT, "Creating the lights' shadow");
            localLightShadow = localLightOutput.texture.Get();
        }
    }

    // The game's models, the textures their meshes use and its terrain, copied before Direct3D 12 may
    // read them.
    endStep(InputsStep);
    gi_renderer::Frame frame;
    engine_scene::Snapshot engineScene;
    engine_scene::take(engineScene, gi_inputs::takeDrawnBones(*sceneCamera));
    for (const auto& model : engineScene.newModels)
        for (const auto& part : model->parts) gi_textures::want(part.baseTexture, part.alphaTested);
    endStep(ModelsStep);
    gi_textures::update(device.Get(), immediate.Get(), frame.newTextures);
    endStep(TexturesStep);
    if (const auto terrain = engine_scene::takeTerrain()) copyTerrain(*terrain, frame);

    // The game's velocity for its moving meshes, for the shadows' and the light's denoisers: only this
    // frame's, drawn before this list, at the render size.
    DXGI_FORMAT velocityFormat{};
    const bool wantsVelocity = shadow || (options.enabled && options.light);
    const bool velocityCopied = wantsVelocity && objectVelocityDrawn && objectVelocity && sizeOf(objectVelocity.Get()) == sizeOf(depth)
        && copyShared(objectVelocity.Get(), objectVelocityCopy, velocityFormat);
    if (wantsVelocity) ++(velocityCopied ? kicksWithObjectVelocity : kicksWithoutObjectVelocity);

    endStep(TerrainStep);
    const auto inputsReady = ++fenceValue;
    check(immediate->Signal(fence.Get(), inputsReady), "Signalling the global illumination's inputs");
    immediate->Flush();
    endStep(SignalStep);

    frame.textures = {depthOut.texture.Get(), normalOut.texture.Get(), albedoOut.texture.Get(), light, shadow, shadowDebug,
        velocityCopied ? objectVelocityCopy.Get() : nullptr, velocityFormat, localLightShadow, lightDebug};
    frame.width = depthOut.width;
    frame.height = depthOut.height;
    frame.viewProjection = projectionViewOf(*sceneCamera);
    frame.previousViewProjection = projectionViewOf(*previousCamera);
    frame.view = rowsOf(*sceneCamera, ViewRows);
    frame.projection = rowsOf(*sceneCamera, ProjectionRows);
    frame.previousView = rowsOf(*previousCamera, ViewRows);
    frame.previousProjection = rowsOf(*previousCamera, ProjectionRows);
    frame.cameraCut = camera::cameraCut(camera::viewOf(*previousCamera), camera::viewOf(*sceneCamera));
    frame.jitter = {sceneCamera->c[JitterRow][0], sceneCamera->c[JitterRow][1], previousCamera->c[JitterRow][0], previousCamera->c[JitterRow][1]};
    for (int i = 0; i < 3; ++i) frame.eye[i] = sceneCamera->c[20][i];

    frame.nearPlane = sceneCamera->c[22][2];
    frame.farPlane = sceneCamera->c[22][3];
    // P11 (c5.y) is 1 / tan(half the vertical field of view).
    frame.verticalFov = 2 * std::atan(1 / std::max(std::abs(sceneCamera->c[5][1]), 1e-3f));
    if (sun)
    {
        frame.sunDirection = sun->direction;
        frame.sunColor = {sun->color[0] * LightScale, sun->color[1] * LightScale, sun->color[2] * LightScale};
    }

    if (sky)
    {
        const auto radiance = gi_lighting::skyRadiance(sky->matrices, LightScale);
        for (int i = 0; i < 9; ++i) frame.skyRadiance[i] = radiance[i];
    }

    // The main view's m_exposure.z, as this frame's lighting passes have it; the sun pass's before
    // the main view's has one.
    const float cameraExposure = sceneCamera->c[23][2];
    const float exposure = cameraExposure > 0 ? cameraExposure : sun ? sun->exposure : 0;
    frame.outputScale = exposure / LightScale;

    frame.newModels = std::move(engineScene.newModels);
    frame.modelInstances = std::move(engineScene.instances);
    frame.characters = std::move(engineScene.characters);
    frame.modelInstancesUpdated = engineScene.instancesUpdated;
    frame.frameIndex = ++kicks;
    frame.showScene = options.showScene;
    frame.logOften = options.logOften;
    frame.traceLight = options.enabled && options.light;
    // The rays need the sun's direction, which the first sun pass gives.
    frame.traceShadows = shadow && sun;
    // Without the shadow map's shadow the rays have no range to leave anything to (BeyondShadowMap).
    frame.shadowRange = options.withShadowMap ? shadowRange : std::array<float, 4>{};
    frame.sunTangent = std::tan(options.sunSize * 0.5f * 3.14159265f / 180);
    frame.debugShadows = shadowDebug != nullptr;
    frame.shadowStarts = options.shadowStarts;
    frame.shadowsFromDepth = options.shadowsFromDepth;
    frame.opaqueLeaves = options.opaqueLeaves;
    frame.lightRays = static_cast<std::uint32_t>(std::clamp(options.lightPixelRays, 1, 4));
    // The lights' colours in the sun's units for the hits (gi_light.hlsl, LocalLightAt).
    for (auto& localLight : localLightsToTrace)
        for (auto& value : localLight.color) value *= LightScale;
    frame.localLights = std::move(localLightsToTrace);
    frame.localLightRays = static_cast<std::uint32_t>(options.lightRays);
    frame.waitValue = inputsReady;
    frame.signalValue = ++fenceValue;
    const auto result = gi_renderer::render(frame);
    endStep(RenderStep);
    lightSignalled = frame.signalValue;
    {
        std::lock_guard lock(lightMutex);
        lightDebugWritten = lightDebug && frame.traceLight && lightDebugOutput.texture.Get() == lightDebug;
    }
    if (frame.traceShadows)
    {
        std::lock_guard lock(lightMutex);
        shadowWritten = shadowOutput.texture.Get() == shadow;
        shadowDebugWritten = shadowDebug && shadowDebugOutput.texture.Get() == shadowDebug;
    }
    if (localLightShadow)
    {
        std::lock_guard lock(lightMutex);
        localLightWritten = localLightOutput.texture.Get() == localLightShadow;
    }

    engine_scene::forget(result.releasedModels);
    gi_textures::release(result.releasedTextures);
    lastKick = GetTickCount64();
    if constexpr (DiagnosticLog) logLights(frame, cameraExposure);
}
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* gameImmediate) noexcept
{
    {
        std::lock_guard lock(executionMutex);
        nativeImmediate = gameImmediate;
        if (FAILED(gameDevice->QueryInterface(IID_PPV_ARGS(&device))) || FAILED(gameImmediate->QueryInterface(IID_PPV_ARGS(&immediate))))
        {
            device.Reset();
            immediate.Reset();
            failed = true;
            failure = "Direct3D 11.4 is missing.";
        }
    }

    std::lock_guard lock(lightMutex);
    lightDevice = gameDevice;
}

void detach() noexcept
{
    enabled = false;
    gi_inputs::setActive(false);
    engine_scene::setActive(false);
    gi_inputs::detach();
    {
        std::lock_guard lock(executionMutex);
        gi_renderer::stop();
        packShader.Reset();
        packParameters.Reset();
        terrainPages.Reset();
        terrainHeights.Reset();
        gi_textures::reset();
        depthOut = normalOut = albedoOut = {};
        normals.Reset();
        albedo.Reset();
        objectVelocity.Reset();
        objectVelocityCopy.Reset();
        objectVelocityDrawn = false;
        fence.Reset();
        if (fenceHandle) CloseHandle(fenceHandle);
        fenceHandle = nullptr;
        fenceValue = lightSignalled = lightWaited = 0;
        sceneCamera.reset();
        previousCamera.reset();
        sky.reset();
        sun.reset();
        shadowRange = {};
        immediate.Reset();
        nativeImmediate = nullptr;
        device.Reset();
        failed = false;
        failure.clear();
    }

    {
        std::lock_guard lock(lightMutex);
        output = shadowOutput = shadowDebugOutput = localLightOutput = lightDebugOutput = {};
        shadowWritten = shadowDebugWritten = localLightWritten = lightDebugWritten = false;
        lightDevice.Reset();
    }

    {
        std::lock_guard lock(notesMutex);
        notes.clear();
    }

    std::unique_lock lock(shaderMutex);
    skyProbeShaders.clear();
    cameraVelocityShaders.clear();
    shaderOwners.clear();
}

void configure(const Options& options) noexcept
{
    std::lock_guard lock(optionsMutex);
    wanted = options;
}

std::string problem()
{
    {
        std::lock_guard lock(optionsMutex);
        if (!wanted.enabled && !wanted.shadows && !wanted.localLights) return {};
    }

    std::lock_guard lock(executionMutex);
    if (failed) return "Ray tracing is unavailable: " + failure;
    if (sinceKick() > 1000) return "Ray tracing is waiting for the game's 3D view.";

    return {};
}

std::string status()
{
    std::string text = engine_scene::status() + "; " + gi_textures::status();
    const auto scene = gi_renderer::status();
    if (!scene.empty()) text += "\n" + scene;

    return text;
}

void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    const auto name = toHex(*checksum);
    if (name != SkyProbeChecksum && name != CameraVelocityChecksum) return;

    try
    {
        std::unique_lock lock(shaderMutex);
        (name == SkyProbeChecksum ? skyProbeShaders : cameraVelocityShaders).insert(shader);
        shaderOwners.emplace_back(shader);
    }
    catch (...)
    {
    }
}

void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    gi_inputs::vertexShaderCreated(code, size, shader);
}

void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept
{
    gi_inputs::vertexShaderBound(context, shader);
}

void bufferMapped(ID3D11Resource* buffer, void* data) noexcept
{
    gi_inputs::bufferMapped(buffer, data);
}

void bufferUnmapped(ID3D11Resource* buffer) noexcept
{
    gi_inputs::bufferUnmapped(buffer);
}

void draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (call.indexed)
    {
        gi_inputs::drawIndexed(context, call);
        return;
    }

    if (call.count != 3 || call.instances != 1) return;

    try
    {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        bool skyProbe{}, cameraVelocity{};
        {
            std::shared_lock lock(shaderMutex);
            skyProbe = skyProbeShaders.contains(shader.Get());
            cameraVelocity = cameraVelocityShaders.contains(shader.Get());
        }

        if (cameraVelocity)
        {
            ComPtr<ID3D11ShaderResourceView> velocity;
            context->PSGetShaderResources(ObjectVelocitySlot, 1, velocity.GetAddressOf());
            std::lock_guard lock(notesMutex);
            notesOf(context).velocity = std::move(velocity);
            return;
        }

        if (!skyProbe) return;

        // PsWork (SH_SphereMap_ps): the probe's light, g_psParamSH (c0 to c11, then its sky occlusion
        // c12 to c15), and the sky's, g_psParamSHSky (c16 to c27); the scene's exposure (c23.z). The
        // game's ambient light is the probe's plus the sky's times the occlusion. Where the game gives no
        // sky (Mother Base: sky and occlusion zero, the probe's light all of it), the probe's light stands
        // in for the sky.
        ComPtr<ID3D11Buffer> scene, probes;
        context->PSGetConstantBuffers(SceneSlot, 1, scene.GetAddressOf());
        context->PSGetConstantBuffers(ProbeSlot, 1, probes.GetAddressOf());
        std::array<float, 120> sceneConstants{};
        std::array<float, 144> probeConstants{};
        if (!gi_inputs::written(scene.Get(), sceneConstants) || !gi_inputs::written(probes.Get(), probeConstants)) return;

        Sky noted;
        noted.fromProbe = std::all_of(probeConstants.begin() + 16 * 4, probeConstants.begin() + 28 * 4, [](float value) { return value == 0.0f; });
        const int first = noted.fromProbe ? 0 : 16;
        for (int color = 0; color < 3; ++color) std::memcpy(noted.matrices[color].data(), &probeConstants[(first + 4 * color) * 4], 16 * sizeof(float));
        noted.exposure = sceneConstants[23 * 4 + 2];
        std::lock_guard lock(notesMutex);
        notesOf(context).sky = noted;
    }
    catch (...)
    {
    }
}

void noteNormals(ID3D11DeviceContext* context, ID3D11ShaderResourceView* view) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        std::lock_guard lock(notesMutex);
        notesOf(context).normals = view;
    }
    catch (...)
    {
    }
}

void noteUpscale(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        std::lock_guard lock(notesMutex);
        auto& noted = notesOf(context);
        noted.kick = true;
        noted.depth = depth;
    }
    catch (...)
    {
    }
}

void noteSun(ID3D11DeviceContext* context, bool shadowMapped) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        ComPtr<ID3D11Buffer> scene, light;
        context->PSGetConstantBuffers(SceneSlot, 1, scene.GetAddressOf());
        context->PSGetConstantBuffers(LightSlot, 1, light.GetAddressOf());
        std::array<float, 120> sceneConstants{};
        std::array<float, 44> lightConstants{};
        if (!gi_inputs::written(scene.Get(), sceneConstants) || !gi_inputs::written(light.Get(), lightConstants)) return;

        // cPSLight c0: towards the sun in view space; c1: its colour, which the pass multiplies by
        // m_exposure.z. The lighting passes' m_view rows (c8 to c10) take view to world.
        std::array<std::array<float, 4>, 3> viewToWorld{};
        for (int row = 0; row < 3; ++row) std::memcpy(viewToWorld[row].data(), &sceneConstants[(8 + row) * 4], 4 * sizeof(float));

        const float exposure = sceneConstants[23 * 4 + 2];
        Sun noted;
        noted.direction = gi_lighting::toWorld(viewToWorld, {lightConstants[0], lightConstants[1], lightConstants[2]});
        noted.color = {lightConstants[4], lightConstants[5], lightConstants[6]};
        noted.exposure = exposure;
        std::lock_guard lock(notesMutex);
        auto& contextNotes = notesOf(context);
        contextNotes.sun = noted;
        // c8.x, c8.y: the range's centre depth and start distance; c6.x: its fade (shadows_shaders.hpp, SunShadow).
        if (shadowMapped) contextNotes.shadowRange = std::array<float, 4>{lightConstants[32], lightConstants[33], lightConstants[24], 1};
        // The sun pass reads the shadows, and the point and spot lights are drawn before its list: it waits for them.
        if (shadowing.load(std::memory_order_relaxed) || localShadowing.load(std::memory_order_relaxed)) contextNotes.wait = true;
    }
    catch (...)
    {
    }
}

void noteSkySpecular(ID3D11DeviceContext* context) noexcept
{
    if (!enabled.load(std::memory_order_relaxed) || !localShadowing.load(std::memory_order_relaxed)) return;

    try
    {
        std::lock_guard lock(notesMutex);
        notesOf(context).wait = true;
    }
    catch (...)
    {
    }
}

void noteResolve(ID3D11DeviceContext* context, ID3D11ShaderResourceView* view) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        std::lock_guard lock(notesMutex);
        auto& noted = notesOf(context);
        noted.wait = true;
        noted.albedo = view;
    }
    catch (...)
    {
    }
}

ID3D11ShaderResourceView* light(UINT width, UINT height) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return nullptr;

    try
    {
        std::lock_guard lock(lightMutex);
        if (!lightDevice) return nullptr;
        resizeShared(output, lightDevice.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the light");

        return output.view.Get();
    }
    catch (const std::exception& error)
    {
        log(std::string("Global illumination: ") + error.what());
        return nullptr;
    }
}

ID3D11ShaderResourceView* localLightShadow(UINT width, UINT height) noexcept
{
    std::lock_guard lock(lightMutex);

    return localLightWritten && localLightOutput.width == width && localLightOutput.height == height ? localLightOutput.view.Get() : nullptr;
}

ID3D11ShaderResourceView* shadowDebug(UINT width, UINT height) noexcept
{
    if (!shadowing.load(std::memory_order_relaxed) || failed || sinceKick() > 1000) return nullptr;

    std::lock_guard lock(lightMutex);
    const bool fits = shadowDebugWritten && shadowDebugOutput.width == width && shadowDebugOutput.height == height;
    return fits ? shadowDebugOutput.view.Get() : nullptr;
}

ID3D11ShaderResourceView* lightDebug(UINT width, UINT height) noexcept
{
    if (!lighting.load(std::memory_order_relaxed) || failed || sinceKick() > 1000) return nullptr;

    std::lock_guard lock(lightMutex);
    const bool fits = lightDebugWritten && lightDebugOutput.width == width && lightDebugOutput.height == height;
    return fits ? lightDebugOutput.view.Get() : nullptr;
}

std::string shadowProbe()
{
    return gi_renderer::shadowProbe();
}

ID3D11ShaderResourceView* sunShadow(UINT width, UINT height) noexcept
{
    if (!shadowing.load(std::memory_order_relaxed) || failed || sinceKick() > 1000) return nullptr;

    try
    {
        std::lock_guard lock(lightMutex);
        if (!lightDevice) return nullptr;
        if (resizeShared(shadowOutput, lightDevice.Get(), width, height, DXGI_FORMAT_R16_FLOAT, "Creating the shadow")) shadowWritten = false;

        return shadowWritten ? shadowOutput.view.Get() : nullptr;
    }
    catch (const std::exception& error)
    {
        log(std::string("Ray-traced shadows: ") + error.what());
        return nullptr;
    }
}

bool active() noexcept
{
    return enabled.load(std::memory_order_relaxed);
}

bool running() noexcept
{
    return lighting.load(std::memory_order_relaxed) && !failed && sinceKick() < 1000;
}

void recorded(std::uint64_t list, std::uint64_t context) noexcept
{
    gi_inputs::recorded(list, context);
    try
    {
        std::lock_guard lock(notesMutex);
        const auto found = notes.find(context);
        if (found == notes.end()) return;

        auto from = std::move(found->second);
        notes.erase(found);
        auto& into = notes[list];
        into.kick |= from.kick;
        into.wait |= from.wait;
        if (from.depth) into.depth = std::move(from.depth);
        if (from.normals) into.normals = std::move(from.normals);
        if (from.albedo) into.albedo = std::move(from.albedo);
        if (from.velocity) into.velocity = std::move(from.velocity);
        if (from.sky) into.sky = from.sky;
        if (from.sun) into.sun = from.sun;
        if (from.shadowRange) into.shadowRange = from.shadowRange;
    }
    catch (...)
    {
    }
}

void forget(std::uint64_t list) noexcept
{
    gi_inputs::forget(list);
    std::lock_guard lock(notesMutex);
    notes.erase(list);
}

void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    Notes noted;
    {
        std::lock_guard lock(notesMutex);
        const auto found = notes.find(list);
        if (found != notes.end())
        {
            noted = std::move(found->second);
            notes.erase(found);
        }
    }

    std::lock_guard lock(executionMutex);
    if (!immediate || failed || context != nativeImmediate) return;

    gi_inputs::executing(list);
    if (noted.sky) sky = noted.sky;
    if (noted.sun) sun = noted.sun;
    if (noted.shadowRange) shadowRange = *noted.shadowRange;
    if (noted.normals) normals = noted.normals;
    if (noted.albedo) albedo = noted.albedo;

    Options options;
    {
        std::lock_guard optionsLock(optionsMutex);
        options = current;
    }

    try
    {
        if (noted.kick)
        {
            LARGE_INTEGER started{}, finished{};
            QueryPerformanceCounter(&started);
            kick(noted.depth.Get(), options);
            QueryPerformanceCounter(&finished);
            kickTicks += finished.QuadPart - started.QuadPart;
            ++kicksSinceLog;
        }
        // The resolve reads the light this frame's kick asked for.
        if (noted.wait && lightSignalled > lightWaited)
        {
            const performance::Timed timed(immediate.Get(), performance::Pass::GiWait);
            check(immediate->Wait(fence.Get(), lightSignalled), "Waiting for the global illumination");
            lightWaited = lightSignalled;
        }
    }
    catch (const std::exception& error)
    {
        fail(error.what());
    }

    // Noted after the kick: a list's velocity is drawn when the list runs, after the kick before it.
    if (noted.velocity)
    {
        objectVelocity = std::move(noted.velocity);
        objectVelocityDrawn = true;
    }
}

void present() noexcept
{
    {
        std::lock_guard lock(executionMutex);
        objectVelocityDrawn = false;
    }

    Options options;
    {
        std::lock_guard lock(optionsMutex);
        current = wanted;
        options = current;
    }

    const bool on = (options.enabled || options.shadows || options.localLights) && !failed;
    lighting = on && options.enabled;
    shadowing = on && options.shadows;
    localShadowing = on && options.localLights;
    // Taking other meshes, the scene starts over and reads the models again.
    const bool otherMeshes = engine_scene::setMainViewMeshes(!options.mainViewMeshesLeftOut) && on;
    if ((enabled.exchange(on) && !on) || otherMeshes)
    {
        // Off gives the GPU memory back; on starts the scene over.
        std::lock_guard lock(executionMutex);
        gi_renderer::stop();
        gi_textures::reset();
        lightSignalled = lightWaited = 0;
    }

    gi_inputs::setActive(on);
    if (otherMeshes) engine_scene::setActive(false);
    engine_scene::setActive(on);
    engine_scene::setBonesFromSnapshot(options.bonesFromSnapshot);
    if constexpr (DiagnosticLog)
    {
        if (on) logStatus();
    }
}
}
