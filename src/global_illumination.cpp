// Ray-traced global illumination; global_illumination.hpp describes the route.
#include "global_illumination.hpp"

#include "camera.hpp"
#include "gi_capture.hpp"
#include "gi_lighting.hpp"
#include "gi_renderer.hpp"
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
#include <mutex>
#include <optional>
#include <shared_mutex>
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
constexpr UINT SceneSlot = 2, LightSlot = 3, ProbeSlot = 7;

// Packs what the kernels read from the G-buffer: the depth, the normals in world space (0 where
// the sky is) and the albedo, linear.
constexpr char PackSource[] = R"(
Texture2D<float> g_Depth : register(t0);
Texture2D<float4> g_Normal : register(t1);
Texture2D<float4> g_Albedo : register(t2);
RWTexture2D<float> g_DepthOut : register(u0);
RWTexture2D<float4> g_NormalOut : register(u1);
RWTexture2D<float4> g_AlbedoOut : register(u2);

cbuffer Parameters : register(b0)
{
    float4 g_View[3]; // World to view, rows
    uint2 g_Size;
    uint g_HasAlbedo;
    uint g_Padding;
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
    g_NormalOut[id] = depth > 0 ? float4(0.5 * world + 0.5, 1) : 0;

    g_AlbedoOut[id] = float4(g_HasAlbedo ? g_Albedo.Load(int3(id, 0)).rgb : 0.25, 1);
}
)";

struct PackParameters
{
    float view[3][4];
    std::uint32_t width, height, hasAlbedo, padding;
};

// The light the global illumination keeps in its caches is the game's before exposure, scaled by
// this: the game's auto-exposure changes quickly, and caches holding exposed light lagged behind it
// and overshot (a white screen). The output takes the frame's exposure (gi_renderer::Frame::outputScale).
constexpr float LightScale = 1.0f / 1024;

// The sky's light as PsWork holds it, before exposure, and the exposure the pass had.
struct Sky
{
    std::array<std::array<float, 16>, 3> matrices{};
    float exposure{};
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
    ComPtr<ID3D11ShaderResourceView> depth, normals, albedo;
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
// The scene runs (for the light, the shadows or both), the light runs, the shadows run.
std::atomic<bool> enabled{}, lighting{}, shadowing{}, failed{};

std::shared_mutex shaderMutex;
std::unordered_set<IUnknown*> skyProbeShaders;
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
std::optional<Sky> sky;
std::optional<Sun> sun;
std::array<float, 4> shadowRange{};
ComPtr<ID3D11ShaderResourceView> normals, albedo;
ComPtr<ID3D11ComputeShader> packShader;
ComPtr<ID3D11Buffer> packParameters;
SharedTexture depthOut, normalOut, albedoOut, transfer;
std::string failure;
std::atomic<std::uint64_t> lastKick{}; // GetTickCount64 of the last frame lit
// Diagnostics: the kicks' CPU time in performance counter ticks, and the kicks, since the last log.
std::atomic<std::int64_t> kickTicks{};
std::atomic<std::uint64_t> kicksSinceLog{};

// The light the resolve reads and the shadow the sun pass reads, made at the render size on
// whichever thread asks first. Taken after executionMutex when both are.
std::mutex lightMutex;
ComPtr<ID3D11Device> lightDevice;
SharedTexture output, shadowOutput;
bool shadowWritten{}; // Direct3D 12 has traced into shadowOutput

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
    gi_capture::setActive(false);
    gi_renderer::stop();
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

    transfer = makeShared(device.Get(), GI_TRANSFER_WIDTH, gi_renderer::TransferVertices / GI_TRANSFER_WIDTH,
        DXGI_FORMAT_R32G32B32A32_FLOAT, "Creating the transfer texture");

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
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(immediate->Map(packParameters.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Writing the G-buffer pack's constants");
    std::memcpy(mapped.pData, &parameters, sizeof(parameters));
    immediate->Unmap(packParameters.Get(), 0);

    ID3D11ShaderResourceView* inputs[]{depth, normals.Get(), hasAlbedo ? albedo.Get() : nullptr};
    ID3D11UnorderedAccessView* outputs[]{depthOut.access.Get(), normalOut.access.Get(), albedoOut.access.Get()};
    immediate->CSSetShader(packShader.Get(), nullptr, 0);
    immediate->CSSetConstantBuffers(0, 1, packParameters.GetAddressOf());
    immediate->CSSetShaderResources(0, 3, inputs);
    immediate->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
    immediate->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView* noInputs[3]{};
    ID3D11UnorderedAccessView* noOutputs[3]{};
    immediate->CSSetShaderResources(0, 3, noInputs);
    immediate->CSSetUnorderedAccessViews(0, 3, noOutputs, nullptr);
    immediate->CSSetShader(nullptr, nullptr, 0);
}

gi_renderer::Matrix projectionViewOf(const camera::SceneConstants& scene)
{
    gi_renderer::Matrix matrix{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) matrix[row * 4 + column] = scene.c[row][column];

    return matrix;
}

// The frame's kick, before the list with the probe upscale runs; call with executionMutex held.
void kick(ID3D11ShaderResourceView* depth, const Options& options)
{
    ensurePipeline();
    // Direct3D 12 is done with the shared inputs of the frame before.
    performance::Timed timed(immediate.Get(), performance::Pass::GiWait);
    if (lightSignalled) check(immediate->Wait(fence.Get(), lightSignalled), "Waiting for the global illumination");

    timed.next(performance::Pass::GiInputs);
    auto converted = gi_capture::convert(immediate.Get(), transfer.access.Get());
    if (converted.camera)
    {
        previousCamera = sceneCamera ? sceneCamera : converted.camera;
        sceneCamera = converted.camera;
    }

    if (!sceneCamera || !depth || !normals)
    {
        // Nothing traces this frame; what it captured is captured again when next drawn.
        std::vector<std::uint32_t> unused;
        for (const auto& capture : converted.captures) unused.push_back(capture.entry);
        gi_capture::evicted(unused);
        return;
    }

    pack(depth, *sceneCamera);
    ID3D11Texture2D* light{};
    ID3D11Texture2D* shadow{};
    {
        std::lock_guard lock(lightMutex);
        if (output.width != depthOut.width || output.height != depthOut.height)
            output = makeShared(device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the light");
        light = output.texture.Get();
        if (options.shadows)
        {
            if (shadowOutput.width != depthOut.width || shadowOutput.height != depthOut.height)
            {
                shadowOutput = makeShared(device.Get(), depthOut.width, depthOut.height, DXGI_FORMAT_R16_FLOAT, "Creating the shadow");
                shadowWritten = false;
            }

            shadow = shadowOutput.texture.Get();
        }
    }

    const auto inputsReady = ++fenceValue;
    check(immediate->Signal(fence.Get(), inputsReady), "Signalling the global illumination's inputs");
    immediate->Flush();

    gi_renderer::Frame frame;
    frame.textures = {depthOut.texture.Get(), normalOut.texture.Get(), albedoOut.texture.Get(), transfer.texture.Get(), light, shadow};
    frame.width = depthOut.width;
    frame.height = depthOut.height;
    frame.viewProjection = projectionViewOf(*sceneCamera);
    frame.previousViewProjection = projectionViewOf(*previousCamera);
    for (int i = 0; i < 3; ++i)
    {
        frame.eye[i] = sceneCamera->c[20][i];
        frame.previousEye[i] = previousCamera->c[20][i];
    }

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

    frame.captures = std::move(converted.captures);
    frame.drawn = std::move(converted.drawn);
    frame.alphaTested = std::move(converted.alphaTested);
    frame.frameIndex = ++kicks;
    frame.showScene = options.showScene;
    frame.traceLight = options.enabled && options.light;
    // The rays need the sun's direction, which the first sun pass gives.
    frame.traceShadows = shadow && sun;
    frame.shadowRange = shadowRange;
    frame.sunTangent = std::tan(options.sunSize * 0.5f * 3.14159265f / 180);
    frame.waitValue = inputsReady;
    frame.signalValue = ++fenceValue;
    const auto result = gi_renderer::render(frame);
    lightSignalled = frame.signalValue;
    if (frame.traceShadows)
    {
        std::lock_guard lock(lightMutex);
        shadowWritten = shadowOutput.texture.Get() == shadow;
    }

    gi_capture::evicted(result.evicted);
    lastKick = GetTickCount64();

    // Diagnostics: the light the scene gets, every ten seconds.
    static std::uint64_t lastLightLog{};
    if (frame.traceLight && lastKick - lastLightLog > 10'000)
    {
        lastLightLog = lastKick;
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
                + ", down " + text(gi_lighting::evaluate(radiance, {0, -1, 0})) + "; the game's sky light up " + text(gameUp);
        }

        log(line + ".");
    }
}
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* gameImmediate) noexcept
{
    gi_capture::attach(gameDevice);
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
    gi_capture::setActive(false);
    gi_capture::detach();
    {
        std::lock_guard lock(executionMutex);
        gi_renderer::stop();
        packShader.Reset();
        packParameters.Reset();
        depthOut = normalOut = albedoOut = transfer = {};
        normals.Reset();
        albedo.Reset();
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
        output = shadowOutput = {};
        shadowWritten = false;
        lightDevice.Reset();
    }

    {
        std::lock_guard lock(notesMutex);
        notes.clear();
    }

    std::unique_lock lock(shaderMutex);
    skyProbeShaders.clear();
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
        if (!wanted.enabled && !wanted.shadows) return {};
    }

    std::lock_guard lock(executionMutex);
    if (failed) return "Ray tracing is unavailable: " + failure;
    const auto last = lastKick.load();
    if (!last || GetTickCount64() - last > 1000) return "Ray tracing is waiting for the game's 3D view.";

    return {};
}

std::string status()
{
    std::string text = gi_capture::status();
    const auto scene = gi_renderer::status();
    if (!scene.empty()) text += "\n" + scene;

    return text;
}

void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader || toHex(*checksum) != SkyProbeChecksum) return;

    try
    {
        std::unique_lock lock(shaderMutex);
        skyProbeShaders.insert(shader);
        shaderOwners.emplace_back(shader);
    }
    catch (...)
    {
    }
}

void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    gi_capture::vertexShaderCreated(code, size, shader);
}

void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept
{
    gi_capture::vertexShaderBound(context, shader);
}

void bufferMapped(ID3D11Resource* buffer, void* data) noexcept
{
    gi_capture::bufferMapped(buffer, data);
}

void bufferUnmapped(ID3D11Resource* buffer) noexcept
{
    gi_capture::bufferUnmapped(buffer);
}

void draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (call.indexed)
    {
        gi_capture::drawIndexed(context, call);
        return;
    }

    if (call.count != 3 || call.instances != 1) return;

    try
    {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        {
            std::shared_lock lock(shaderMutex);
            if (!skyProbeShaders.contains(shader.Get())) return;
        }

        // PsWork's g_psParamSHSky (c16 to c27) and the scene's exposure (c23.z).
        ComPtr<ID3D11Buffer> scene, probes;
        context->PSGetConstantBuffers(SceneSlot, 1, scene.GetAddressOf());
        context->PSGetConstantBuffers(ProbeSlot, 1, probes.GetAddressOf());
        std::array<float, 120> sceneConstants{};
        std::array<float, 144> probeConstants{};
        if (!gi_capture::written(scene.Get(), sceneConstants) || !gi_capture::written(probes.Get(), probeConstants)) return;

        Sky noted;
        for (int color = 0; color < 3; ++color) std::memcpy(noted.matrices[color].data(), &probeConstants[(16 + 4 * color) * 4], 16 * sizeof(float));
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
        if (!gi_capture::written(scene.Get(), sceneConstants) || !gi_capture::written(light.Get(), lightConstants)) return;

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
        // The sun pass reads the shadows: its list waits for them.
        if (shadowing.load(std::memory_order_relaxed)) contextNotes.wait = true;
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
        if (output.width != width || output.height != height)
            output = makeShared(lightDevice.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the light");

        return output.view.Get();
    }
    catch (const std::exception& error)
    {
        log(std::string("Global illumination: ") + error.what());
        return nullptr;
    }
}

ID3D11ShaderResourceView* sunShadow(UINT width, UINT height) noexcept
{
    const auto last = lastKick.load(std::memory_order_relaxed);
    if (!shadowing.load(std::memory_order_relaxed) || failed || !last || GetTickCount64() - last > 1000) return nullptr;

    try
    {
        std::lock_guard lock(lightMutex);
        if (!lightDevice) return nullptr;
        if (shadowOutput.width != width || shadowOutput.height != height)
        {
            shadowOutput = makeShared(lightDevice.Get(), width, height, DXGI_FORMAT_R16_FLOAT, "Creating the shadow");
            shadowWritten = false;
        }

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
    const auto last = lastKick.load(std::memory_order_relaxed);
    return lighting.load(std::memory_order_relaxed) && !failed && last && GetTickCount64() - last < 1000;
}

void recorded(std::uint64_t list, std::uint64_t context) noexcept
{
    gi_capture::recorded(list, context);
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
    gi_capture::forget(list);
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

    gi_capture::executing(list);
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
}

void present() noexcept
{
    Options options;
    {
        std::lock_guard lock(optionsMutex);
        current = wanted;
        options = current;
    }

    const bool on = (options.enabled || options.shadows) && !failed;
    lighting = on && options.enabled;
    shadowing = on && options.shadows;
    if (enabled.exchange(on) && !on)
    {
        // Off gives the GPU memory back; on starts the scene over.
        std::lock_guard lock(executionMutex);
        gi_renderer::stop();
        lightSignalled = lightWaited = 0;
    }

    gi_capture::setActive(on);
    gi_capture::setCapturing(options.capture);

    // What the scene holds, and the CPU work a frame, every ten seconds while it runs.
    static std::uint64_t lastStatus{};
    const auto now = GetTickCount64();
    if (on && now - lastStatus > 10'000)
    {
        lastStatus = now;
        log("Global illumination: " + status() + "; " + std::to_string(gi_capture::nearCopies()) + " meshes held twice within 4 cm of themselves");

        const auto work = gi_capture::takeWork();
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const double frames = static_cast<double>(std::max<std::uint64_t>(kicksSinceLog.exchange(0), 1));
        const double kick = static_cast<double>(kickTicks.exchange(0)) * 1000.0 / static_cast<double>(frequency.QuadPart);
        const auto perFrame = [&](double value) { return std::to_string(value / frames); };
        log("Global illumination's CPU time a frame: hooks " + perFrame(work.hookMilliseconds) + " ms (all threads) over " + perFrame(static_cast<double>(work.draws))
            + " mesh draws, with " + perFrame(static_cast<double>(work.captures)) + " captures (" + perFrame(static_cast<double>(work.skinned)) + " skinned, "
            + perFrame(static_cast<double>(work.vertices) / 1000) + "k vertices); the kick " + perFrame(kick) + " ms (render thread).");
    }
}
}
