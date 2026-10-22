#include "shadows.hpp"

#include "global_illumination.hpp"
#include "log.hpp"
#include "performance.hpp"
#include "shader_bytecode.hpp"
#include "shadows_shaders.hpp"

#include <bend_sss/bend_sss_cpu.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace phantom_fx::shadows
{
namespace
{
using Microsoft::WRL::ComPtr;

// The game's sun light pixel shaders (docs/DEVELOPMENT.md, "Sun light and shadows"): inside the
// shadow range, with the shadow map, and beyond it. In rain the game draws a pair without the sky
// reflection, as its rain filter reflects the sky instead.
enum class Sun : std::uint8_t { ShadowMap, Far, RainShadowMap, RainFar, Count, None = Count };
constexpr const char* SunChecksums[]{
    "56ed4db892a7940f038146eeb0df0d30", // SunLight_DL_CS_ES
    "92726e8d231c8ed85f0db90400ef2258", // SunLight_DL_ES
    "7e5c2fc99c9d84ddd32df4e789e7082a", // SunLight_DL_CS
    "853f56a63e5bc66e6ee301c85ebfeaa8", // SunLight_DL
};
constexpr const char* SunNames[]{"shadow map", "far", "shadow map, rain", "far, rain"};
static_assert(std::size(SunChecksums) == static_cast<std::size_t>(Sun::Count) && std::size(SunNames) == std::size(SunChecksums));

bool shadowMapped(Sun sun)
{
    return sun == Sun::ShadowMap || sun == Sun::RainShadowMap;
}

bool reflectsSky(Sun sun)
{
    return sun == Sun::ShadowMap || sun == Sun::Far;
}

// How a local light's shader fades its light to zero with the distance d to the light, from the
// disassembly. The points add cPSLight c6.x to d; the spots do not.
// - Quartic (SSLighting2_*): 1 / d^2 - d^2 c6.w, zero at d = c6.w^(-1/4).
// - Sixteenth (Lighting_*_NA): (1 - d^16 c6.z) / d^2, zero at d = c6.z^(-1/16).
enum class Falloff : std::uint8_t { Quartic, Sixteenth };

// The game's point and spot light pixel shaders (docs/DEVELOPMENT.md, "Local lights"), all with the
// light's view-space position in cPSLight c3.
struct LocalLightShader
{
    const char* checksum;
    Falloff falloff;
};
constexpr LocalLightShader LocalLightShaders[]{
    {"e61cc726c5c877555261f03dbc4aa967", Falloff::Sixteenth}, // Lighting_Point_NA
    {"32bdd328651cf4f11ebcc47c0b65dfde", Falloff::Sixteenth}, // Lighting_Point_NA_NShdw
    {"f037f71a002e42f0cb07ad2768f7b8ac", Falloff::Sixteenth}, // Lighting_Point_NA_NoSpec
    {"259fff504d9caef06a467ff6ce9ee1bf", Falloff::Sixteenth}, // Lighting_Point_NA_NoSpec_NShdw
    {"57b7365fcfb89c3f91fffcebf1a6b1e5", Falloff::Sixteenth}, // Lighting_Spot_NA
    {"d53793289e8c2824800876d8525aab2c", Falloff::Sixteenth}, // Lighting_Spot_NA_Mask
    {"dcc81c5f2c97e20003a27a9e7db54376", Falloff::Sixteenth}, // Lighting_Spot_NA_Mask_NShdw
    {"a435549d3334a5715c165a68083ea66e", Falloff::Sixteenth}, // Lighting_Spot_NA_Mask_NSpec
    {"fd570742642f09e699d193ffb02bf0df", Falloff::Sixteenth}, // Lighting_Spot_NA_Mask_NSpec_NShdw
    {"dddab560572d7352181cd83f00f6dd63", Falloff::Sixteenth}, // Lighting_Spot_NA_NShdw
    {"7d2ed256fcff999707444bb115321c7f", Falloff::Sixteenth}, // Lighting_Spot_NA_NoSpec
    {"91bc5dd518df1368e60d53c5f065e9f4", Falloff::Sixteenth}, // Lighting_Spot_NA_NoSpec_NShdw
    {"afbc12167037e4e0d653da184c7e3e82", Falloff::Quartic},   // SSLighting2_Point
    {"4eb687f92330ee7b11ccf8e32950e4a9", Falloff::Quartic},   // SSLighting2_Point_NoShadow
    {"a1c56a0978c706222ebeacbf6fc9ebe4", Falloff::Quartic},   // SSLighting2_Point_NoSpec
    {"e9cd14a10e3f1df895cb6799c232f99e", Falloff::Quartic},   // SSLighting2_Point_NoSpec_NoShadow
    {"ac0947110cb0f17f4ffe930a6a11617d", Falloff::Quartic},   // SSLighting2_Spot
    {"9ead0f83217db6af698f860fd3233441", Falloff::Quartic},   // SSLighting2_Spot_Mask
    {"2d985165a56e0622d42f086a28c305d0", Falloff::Quartic},   // SSLighting2_Spot_Mask_NShdw
    {"fa6ad11cfc35a2281a22b92c1a2aa6db", Falloff::Quartic},   // SSLighting2_Spot_Mask_NSpec
    {"74872741b699fdefa13ae71349f41ec9", Falloff::Quartic},   // SSLighting2_Spot_Mask_NSpec_NShdw
    {"74063ef2a991625184fdf283ba58c43f", Falloff::Quartic},   // SSLighting2_Spot_NoShadow
    {"cdc559ec2ca4d52a54697cf5ca023667", Falloff::Quartic},   // SSLighting2_Spot_NoSpec
    {"a823ffb58661c508e6270227d4910c91", Falloff::Quartic},   // SSLighting2_Spot_NoSpec_NoShadow
};

// cPSLight (b3) and cPSScene (b2), as the light passes bind them.
constexpr UINT LightConstantsSize = 176, SceneConstantsSize = 480;
constexpr std::size_t LightDirection = 0;                              // c0: towards the sun, view space
constexpr std::size_t LightPosition = 3 * 4;                           // c3: a local light's position, view space
constexpr std::size_t LightFade = 6 * 4;                               // c6: a local light's falloff (see Falloff)
constexpr std::size_t ProjectionParam = 21 * 4, CenterOffset = 28 * 4; // c21: 1 / P00, 1 / P11, B, A; c28: the jitter in NDC
constexpr UINT SpecularSlot = 1;                                       // t1 of the light passes, whose z is the material
constexpr UINT DepthSlot = 2;                                          // t2 of the light passes
constexpr UINT RayTracedSlot = 6;                                      // t6 of the replacement sun shaders
constexpr UINT ShadowSlot = 7;                                         // t7 of the add-on's pixel shaders
constexpr UINT OptionsSlot = 6;                                        // b6 of the add-on's pixel shaders
constexpr int WaveSize = 64;                                           // WAVE_SIZE in shaders::BendShadow
constexpr std::uint32_t PatternTurns = 64;                             // Frames before the soft shadows' samples repeat
// A local light this close to the camera plane is moved off it, so its place on screen stays finite.
constexpr float MinLightDepth = 0.01f;

// What a trace covers: pixels from `min` to `max` (exclusive) whose depth lies between the depth
// bounds (exclusive). It writes no others.
struct TraceBounds
{
    std::int32_t min[2], max[2];
    float depthMin, depthMax;
};

// BendShadow's constant buffer, which the light shadow shader reads too.
struct Parameters
{
    float lightCoordinate[4];
    std::int32_t waveOffset[2];
    float inverseDepthSize[2];
    float surfaceThickness, bilinearThreshold, shadowContrast, padding;
    float depthBounds[2];
    std::int32_t boundsMin[2], boundsMax[2];
    float padding2[2];
};
static_assert(sizeof(Parameters) % 16 == 0);

// Bend's recommended starting values, which suit Fox Engine's reversed depth as they are.
constexpr float SurfaceThickness = 0.005f, BilinearThreshold = 0.02f, ShadowContrast = 4;

// cSunShadows (b6) of the replacement shaders.
struct ShaderOptions
{
    float soft, sunTangent, frame, screenSpace, rayTraced, padding[3];
};
static_assert(sizeof(ShaderOptions) % 16 == 0);

// What the overlay asked for, and what the frame being recorded uses.
std::mutex optionsMutex;
Options wanted, current;
std::atomic<bool> active{}, pipelineFailed{};
// Only the screen-space trace reads the constants bufferMapped keeps.
std::atomic<bool> tracing{};
// Counts presents, to turn the soft shadows' samples.
std::atomic<std::uint32_t> frame{};

// Identified sun shaders, read lock-free on every full-screen draw. The game can create a shader more
// than once; holding the copies keeps their addresses from being reused.
constexpr std::size_t CopiesPerShader = 4;
std::mutex shaderMutex;
std::array<std::array<std::atomic<IUnknown*>, CopiesPerShader>, std::size(SunChecksums)> sunShaders{};
std::array<std::array<ComPtr<IUnknown>, CopiesPerShader>, std::size(SunChecksums)> shaderOwners;

// Identified local light shaders, looked up on every indexed draw.
std::shared_mutex lightShaderMutex;
std::unordered_map<IUnknown*, Falloff> lightShaders;
std::vector<ComPtr<IUnknown>> lightShaderOwners;

// What a thread last wrote to each constant buffer of the two sizes: the game writes them with
// Map, memcpy and Unmap right before the draw that reads them, on the thread recording it.
thread_local ID3D11Resource* mappedBuffer{};
thread_local void* mappedData{};
thread_local UINT mappedSize{};
thread_local std::unordered_map<ID3D11Resource*, std::vector<float>> written;

// The depth a context's recording traced the sun from since its last FinishCommandList: both parts
// of the sun pass read the same shadow. And the light whose shadow a context's last stencil-marking
// draw put in the light targets' alpha, by its position, for the lighting draw that follows.
std::mutex tracedMutex;
std::unordered_map<ID3D11DeviceContext*, ID3D11ShaderResourceView*> traced;
std::unordered_map<ID3D11DeviceContext*, std::array<float, 3>> marked;

// A screen-space shadow, 1 where the light reaches, at the depth buffer's size.
struct ShadowTarget
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> access;
    UINT width{}, height{};
};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> traceShader;
std::array<ComPtr<ID3D11PixelShader>, std::size(SunChecksums)> replacements;
ComPtr<ID3D11PixelShader> lightShadowShader;
ComPtr<ID3D11SamplerState> borderSampler;
ComPtr<ID3D11Buffer> parameterBuffer, optionsBuffer;
// The light shadow's draw: alpha only, over the whole light volume.
ComPtr<ID3D11BlendState> alphaOnly;
ComPtr<ID3D11DepthStencilState> noDepth;
ComPtr<ID3D11RasterizerState> bothFaces;
// The game's additive light blend, by the state it binds, and the same scaled by the target's alpha.
std::unordered_map<ID3D11BlendState*, std::pair<ComPtr<ID3D11BlendState>, ComPtr<ID3D11BlendState>>> shadowedBlends;
ShadowTarget sunShadow, lightShadow;
bool tracedSunOnce{};
std::array<bool, 2> tracedLightOnce{}; // By falloff
std::map<std::string, bool, std::less<>> loggedOnce;

void logOnce(const std::string& message)
{
    if (loggedOnce.emplace(message, true).second) log("Shadows: " + message);
}

void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
}

ComPtr<ID3DBlob> compile(const char* source, const char* name, const char* profile, const D3D_SHADER_MACRO* defines = nullptr)
{
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source, std::strlen(source), name, defines, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error(std::string("Shader ") + name + " did not compile: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                      : hex(static_cast<std::uint32_t>(result))));

    return code;
}

// Compiles the shaders once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (traceShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        auto code = compile(shaders::BendShadow, "bend-screen-space-shadows", "cs_5_0");
        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, traceShader.ReleaseAndGetAddressOf()),
            "Creating the shadow trace");

        for (std::size_t index = 0; index < replacements.size(); ++index)
        {
            const auto sun = static_cast<Sun>(index);
            const D3D_SHADER_MACRO defines[]{{"SHADOW_MAP", shadowMapped(sun) ? "1" : "0"}, {"SKY_REFLECTION", reflectsSky(sun) ? "1" : "0"}, {}};
            code = compile(shaders::SunLight, "sun-light", "ps_5_0", defines);
            check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, replacements[index].ReleaseAndGetAddressOf()),
                "Creating a sun light shader");
        }

        code = compile(shaders::LightShadow, "light-shadow", "ps_5_0");
        check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, lightShadowShader.ReleaseAndGetAddressOf()),
            "Creating the light shadow shader");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER; // Border colour 0: the far plane
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, borderSampler.ReleaseAndGetAddressOf()), "Creating the shadow trace sampler");

        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Parameters);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(device->CreateBuffer(&buffer, nullptr, parameterBuffer.ReleaseAndGetAddressOf()), "Creating the shadow trace constants");
        buffer.ByteWidth = sizeof(ShaderOptions);
        check(device->CreateBuffer(&buffer, nullptr, optionsBuffer.ReleaseAndGetAddressOf()), "Creating the sun shadow options");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        check(device->CreateBlendState(&blend, alphaOnly.ReleaseAndGetAddressOf()), "Creating the light shadow blend");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        check(device->CreateDepthStencilState(&depth, noDepth.ReleaseAndGetAddressOf()), "Creating the light shadow depth state");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&rasterizer, bothFaces.ReleaseAndGetAddressOf()), "Creating the light shadow rasterizer state");

        log("Shadow shaders ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        traceShader.Reset();
        log(std::string("Shadows unavailable: ") + error.what());

        return false;
    }
}

// `target` at `width` x `height`; call with resourceMutex held.
void ensureShadowTarget(ShadowTarget& target, UINT width, UINT height)
{
    if (target.texture && target.width == width && target.height == height) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ShadowTarget created{.width = width, .height = height};
    check(device->CreateTexture2D(&desc, nullptr, created.texture.GetAddressOf()), "Creating the screen-space shadow");
    check(device->CreateShaderResourceView(created.texture.Get(), nullptr, created.view.GetAddressOf()), "Creating the screen-space shadow view");
    check(device->CreateUnorderedAccessView(created.texture.Get(), nullptr, created.access.GetAddressOf()),
        "Creating the screen-space shadow output");
    // Command lists that still use the previous texture hold their own references.
    target = std::move(created);
    log("Screen-space shadows at " + std::to_string(width) + "x" + std::to_string(height) + ".");
}

Sun sunOf(IUnknown* shader)
{
    if (!shader) return Sun::None;

    for (std::size_t sun = 0; sun < sunShaders.size(); ++sun)
        for (const auto& copy : sunShaders[sun])
            if (copy.load(std::memory_order_relaxed) == shader) return static_cast<Sun>(sun);

    return Sun::None;
}

// A local light shader's falloff; none for any other shader.
std::optional<Falloff> falloffOf(IUnknown* shader)
{
    std::shared_lock lock(lightShaderMutex);
    const auto found = shader ? lightShaders.find(shader) : lightShaders.end();
    if (found == lightShaders.end()) return std::nullopt;

    return found->second;
}

// What this thread last wrote to `buffer`, when it has the expected size.
const float* writtenTo(ID3D11Buffer* buffer, UINT size)
{
    const auto found = buffer ? written.find(buffer) : written.end();
    return found != written.end() && found->second.size() * sizeof(float) == size ? found->second.data() : nullptr;
}

// What this thread wrote to the light pass's cPSScene (b2) and cPSLight (b3), null where unknown.
std::pair<const float*, const float*> passConstants(ID3D11DeviceContext* context)
{
    std::array<ID3D11Buffer*, 2> constants{};
    context->PSGetConstantBuffers(2, 2, constants.data());
    const auto* scene = writtenTo(constants[0], SceneConstantsSize);
    const auto* light = writtenTo(constants[1], LightConstantsSize);
    for (auto* buffer : constants)
        if (buffer) buffer->Release();

    return {scene, light};
}

// Bend's inLightProjection: a view-space point (w 1) or direction (w 0) through the pass's own
// projection, c21 and c28, which it reconstructs positions from, so it matches the depth.
std::array<float, 4> project(const float* scene, const float (&light)[4])
{
    const float* projection = scene + ProjectionParam;
    const float* offset = scene + CenterOffset;

    return {light[0] / projection[0] + offset[0] * light[2], light[1] / projection[1] + offset[1] * light[2],
        projection[3] * light[2] + projection[2] * light[3], light[2]};
}

// How far a local light reaches, from its falloff in cPSLight c6; infinite or NaN where unknown.
float rangeOf(const float* light, Falloff falloff)
{
    const float* fade = light + LightFade;
    const float reach = falloff == Falloff::Quartic ? std::pow(fade[3], -1.0f / 4) : std::pow(fade[2], -1.0f / 16);

    // A point adds c6.x to the distance and a spot does not; the larger range covers both.
    return reach - std::min(fade[0], 0.0f);
}

// The width and height of the texture `view` shows.
std::array<int, 2> sizeOf(ID3D11ShaderResourceView* view)
{
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    check(resource.As(&texture), "Reading the depth buffer");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    return {static_cast<int>(desc.Width), static_cast<int>(desc.Height)};
}

// What a light at view-space `center` that reaches `range` can light on a screen of `size`: its
// sphere's bounds on screen and in depth, the whole screen when the range is unknown. None when the
// sphere is behind the camera or off the screen.
std::optional<TraceBounds> boundsOf(const float* scene, const std::array<float, 3>& center, float range, std::array<int, 2> size)
{
    // Pixels at any depth but the sky's.
    TraceBounds bounds{{0, 0}, {size[0], size[1]}, 0, 2};
    if (!std::isfinite(range) || range <= 0) return bounds;

    const float z = center[2];
    if (z + range <= 0) return std::nullopt;

    // The light passes reconstruct the view depth as z = B / (depth - A).
    const float* projection = scene + ProjectionParam;
    const auto depthAt = [&](float viewZ) { return projection[2] / viewZ + projection[3]; };
    bounds.depthMin = std::max(depthAt(z + range), 0.0f);

    // With the camera plane through the sphere, it covers the whole screen.
    if (z - range <= 0) return bounds;

    bounds.depthMax = depthAt(z - range);

    // On each axis, the planes through the eye that touch the sphere, at x / z (or y / z) = t, bound
    // it on screen; pixels as Bend places the light.
    const float* offset = scene + CenterOffset;
    const float clearance = z * z - range * range;
    for (int axis = 0; axis < 2; ++axis)
    {
        const float across = center[axis];
        const float spread = range * std::sqrt(across * across + clearance);
        const float scale = axis == 0 ? 0.5f : -0.5f;
        const auto pixelAt = [&](float t) { return ((t / projection[axis] + offset[axis]) * scale + 0.5f) * static_cast<float>(size[axis]); };
        const float first = pixelAt((across * z - spread) / clearance), second = pixelAt((across * z + spread) / clearance);
        const auto clamped = [&](float pixel) { return static_cast<int>(std::clamp(pixel, 0.0f, static_cast<float>(size[axis]))); };
        bounds.min[axis] = clamped(std::floor(std::min(first, second)) - 1);
        bounds.max[axis] = clamped(std::ceil(std::max(first, second)) + 1);
        if (bounds.min[axis] >= bounds.max[axis]) return std::nullopt;
    }

    return bounds;
}

// Traces the screen-space shadow towards `lightProjection` from `depth` into `target` on `context`,
// with the game's compute state saved around it; `specular` (may be null) tells foliage, which casts
// none. With `bounds`, it writes only within them, and the shadow draw takes the rest as lit;
// without, it covers the whole screen and the sky stays lit. Call with resourceMutex held. Returns
// Bend's dispatch list.
Bend::DispatchList trace(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* specular,
    std::array<float, 4> lightProjection, const std::optional<TraceBounds>& bounds, ShadowTarget& target)
{
    auto size = sizeOf(depth);
    ensureShadowTarget(target, static_cast<UINT>(size[0]), static_cast<UINT>(size[1]));

    // Bend's default depth bounds skip the sky.
    const auto covered = bounds.value_or(TraceBounds{{0, 0}, {size[0], size[1]}, 0, 1});
    int boundsMin[2]{covered.min[0], covered.min[1]}, boundsMax[2]{covered.max[0], covered.max[1]};
    const auto dispatches = Bend::BuildDispatchList(lightProjection.data(), size.data(), boundsMin, boundsMax, false, WaveSize);

    ComPtr<ID3D11ComputeShader> gameShader;
    ComPtr<ID3D11Buffer> gameBuffer;
    std::array<ID3D11ShaderResourceView*, 2> gameViews{};
    ComPtr<ID3D11UnorderedAccessView> gameAccess;
    ComPtr<ID3D11SamplerState> gameSampler;
    context->CSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->CSGetConstantBuffers(0, 1, gameBuffer.GetAddressOf());
    context->CSGetShaderResources(0, static_cast<UINT>(gameViews.size()), gameViews.data());
    context->CSGetUnorderedAccessViews(0, 1, gameAccess.GetAddressOf());
    context->CSGetSamplers(0, 1, gameSampler.GetAddressOf());

    if (!bounds)
    {
        const float lit[4]{1, 1, 1, 1};
        context->ClearUnorderedAccessViewFloat(target.access.Get(), lit);
    }

    context->CSSetShader(traceShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* inputs[]{depth, specular};
    context->CSSetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
    context->CSSetUnorderedAccessViews(0, 1, target.access.GetAddressOf(), nullptr);
    context->CSSetSamplers(0, 1, borderSampler.GetAddressOf());
    context->CSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
    for (int i = 0; i < dispatches.DispatchCount; ++i)
    {
        const auto& dispatch = dispatches.Dispatch[i];
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(parameterBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) break;

        Parameters parameters{};
        std::memcpy(parameters.lightCoordinate, dispatches.LightCoordinate_Shader, sizeof(parameters.lightCoordinate));
        parameters.waveOffset[0] = dispatch.WaveOffset_Shader[0];
        parameters.waveOffset[1] = dispatch.WaveOffset_Shader[1];
        parameters.inverseDepthSize[0] = 1.0f / static_cast<float>(size[0]);
        parameters.inverseDepthSize[1] = 1.0f / static_cast<float>(size[1]);
        parameters.surfaceThickness = SurfaceThickness;
        parameters.bilinearThreshold = BilinearThreshold;
        parameters.shadowContrast = ShadowContrast;
        parameters.depthBounds[0] = covered.depthMin;
        parameters.depthBounds[1] = covered.depthMax;
        std::memcpy(parameters.boundsMin, covered.min, sizeof(parameters.boundsMin));
        std::memcpy(parameters.boundsMax, covered.max, sizeof(parameters.boundsMax));
        std::memcpy(mapped.pData, &parameters, sizeof(parameters));
        context->Unmap(parameterBuffer.Get(), 0);
        context->Dispatch(static_cast<UINT>(dispatch.WaveCount[0]), static_cast<UINT>(dispatch.WaveCount[1]), static_cast<UINT>(dispatch.WaveCount[2]));
    }

    context->CSSetShader(gameShader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, gameBuffer.GetAddressOf());
    context->CSSetShaderResources(0, static_cast<UINT>(gameViews.size()), gameViews.data());
    for (auto* view : gameViews)
        if (view) view->Release();
    context->CSSetUnorderedAccessViews(0, 1, gameAccess.GetAddressOf(), nullptr);
    context->CSSetSamplers(0, 1, gameSampler.GetAddressOf());

    return dispatches;
}

// Where a trace's light is on screen and how many dispatches it took, for the log.
std::string describe(const Bend::DispatchList& dispatches)
{
    return "light at pixel " + std::to_string(dispatches.LightCoordinate_Shader[0]) + ", " + std::to_string(dispatches.LightCoordinate_Shader[1])
        + ", " + std::to_string(dispatches.DispatchCount) + " dispatches";
}

// Whether `context` traced the sun from `depth` since its last FinishCommandList.
bool tracedFrom(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth)
{
    std::lock_guard lock(tracedMutex);
    const auto found = traced.find(context);
    return found != traced.end() && found->second == depth;
}

void noteTraced(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth)
{
    std::lock_guard lock(tracedMutex);
    traced[context] = depth;
}

void noteMarked(ID3D11DeviceContext* context, const std::array<float, 3>& position)
{
    std::lock_guard lock(tracedMutex);
    marked[context] = position;
}

// Whether `context`'s last marking draw was for the light at `position`; it is then used up.
bool takeMarked(ID3D11DeviceContext* context, const std::array<float, 3>& position)
{
    std::lock_guard lock(tracedMutex);
    const auto found = marked.find(context);
    if (found == marked.end()) return false;

    const bool same = found->second == position;
    marked.erase(found);

    return same;
}

// Writes the replacement shaders' options on `context`; call with resourceMutex held. With the
// ray-traced shadow, the shadow map's part is always soft.
void writeOptions(ID3D11DeviceContext* context, const Options& options, bool rayTraced)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(optionsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Writing the sun shadow options");

    constexpr float DegreesToRadians = 3.14159265f / 180;
    const ShaderOptions values{options.soft || rayTraced ? 1.0f : 0.0f, std::tan(options.sunSize * 0.5f * DegreesToRadians),
        options.turnPattern ? static_cast<float>(frame.load(std::memory_order_relaxed) % PatternTurns) : 0.0f, options.screenSpaceSun ? 1.0f : 0.0f,
        rayTraced ? 1.0f : 0.0f, {}};
    std::memcpy(mapped.pData, &values, sizeof(values));
    context->Unmap(optionsBuffer.Get(), 0);
}

// The game's additive light blend (ONE, ONE) with its light scaled by the target's alpha, which it
// keeps; null for any other blend. Call with resourceMutex held.
ID3D11BlendState* shadowedBlend(ID3D11BlendState* game)
{
    if (!game) return nullptr;

    if (const auto found = shadowedBlends.find(game); found != shadowedBlends.end()) return found->second.second.Get();

    D3D11_BLEND_DESC desc{};
    game->GetDesc(&desc);
    const UINT targets = desc.IndependentBlendEnable ? 2 : 1;
    ComPtr<ID3D11BlendState> shadowed;
    for (UINT i = 0; i < targets; ++i)
    {
        auto& target = desc.RenderTarget[i];
        if (!target.BlendEnable || target.SrcBlend != D3D11_BLEND_ONE || target.DestBlend != D3D11_BLEND_ONE || target.BlendOp != D3D11_BLEND_OP_ADD)
        {
            shadowedBlends.emplace(game, std::pair{ComPtr<ID3D11BlendState>(game), ComPtr<ID3D11BlendState>()});
            return nullptr;
        }

        target.SrcBlend = D3D11_BLEND_DEST_ALPHA;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }

    check(device->CreateBlendState(&desc, shadowed.GetAddressOf()), "Creating a shadowed light blend");
    shadowedBlends.emplace(game, std::pair{ComPtr<ID3D11BlendState>(game), shadowed});

    return shadowed.Get();
}

// The sun pass, drawn with the replacement shaders.
bool drawSun(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, Sun sun, ID3D11PixelShader* gameShader, const Options& options)
{
    ComPtr<ID3D11ShaderResourceView> depth, specular;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    context->PSGetShaderResources(SpecularSlot, 1, specular.GetAddressOf());

    // The ray-traced shadow at the depth's size, once Direct3D 12 has traced one; until then the
    // game's shadows stay.
    ID3D11ShaderResourceView* rayTraced{};
    if (options.rayTraced && depth)
    {
        const auto size = sizeOf(depth.Get());
        rayTraced = global_illumination::sunShadow(static_cast<UINT>(size[0]), static_cast<UINT>(size[1]));
    }

    // Beyond the shadow range there is no shadow map to soften.
    if (!options.screenSpaceSun && !(options.soft && shadowMapped(sun)) && !rayTraced) return false;

    const auto [scene, light] = passConstants(context);

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    if (options.screenSpaceSun)
    {
        if (!depth || !scene || !light)
        {
            logOnce("a sun pass without its depth or constants keeps the game's shadows.");
            return false;
        }

        if (!tracedFrom(context, depth.Get()))
        {
            const performance::Timed timed(context, performance::Pass::SunTrace);
            const float* direction = light + LightDirection;
            const auto where = describe(trace(context, depth.Get(), specular.Get(), project(scene, {direction[0], direction[1], direction[2], 0}), {}, sunShadow));
            noteTraced(context, depth.Get());
            if (!tracedSunOnce)
            {
                tracedSunOnce = true;
                log("Shadows: first sun trace, light direction " + std::to_string(direction[0]) + ", " + std::to_string(direction[1]) + ", "
                    + std::to_string(direction[2]) + " (view space), " + where + ".");
            }
        }

        // The diagnostics view lights everything as beyond the shadow range, with the trace's shadow.
        if (options.onlyScreenSpace) sun = reflectsSky(sun) ? Sun::Far : Sun::RainFar;
    }

    // The same for the rays' shadow alone.
    if (rayTraced && options.onlyRayTraced) sun = reflectsSky(sun) ? Sun::Far : Sun::RainFar;

    writeOptions(context, options, rayTraced != nullptr);
    ComPtr<ID3D11ShaderResourceView> gameView, gameRayTracedView;
    ComPtr<ID3D11Buffer> gameOptions;
    context->PSGetShaderResources(ShadowSlot, 1, gameView.GetAddressOf());
    context->PSGetShaderResources(RayTracedSlot, 1, gameRayTracedView.GetAddressOf());
    context->PSGetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
    context->PSSetShader(replacements[static_cast<std::size_t>(sun)].Get(), nullptr, 0);
    context->PSSetShaderResources(ShadowSlot, 1, sunShadow.view.GetAddressOf());
    context->PSSetShaderResources(RayTracedSlot, 1, &rayTraced);
    context->PSSetConstantBuffers(OptionsSlot, 1, optionsBuffer.GetAddressOf());
    performance::Timed timed(context, performance::Pass::SunLight);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    timed.end();
    context->PSSetShader(gameShader, nullptr, 0);
    context->PSSetShaderResources(ShadowSlot, 1, gameView.GetAddressOf());
    context->PSSetShaderResources(RayTracedSlot, 1, gameRayTracedView.GetAddressOf());
    context->PSSetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
    if (options.soft) logOnce("the sun pass draws with soft shadows.");
    if (rayTraced) logOnce("the sun pass draws with ray-traced shadows.");

    return true;
}

// A point or spot light's draws. At the one that marks the stencil (colour writes off), the light's
// shadow is traced within the light's reach, and the light's volume drawn once more, both faces and
// without depth test, to put the shadow in the alpha of both light targets wherever the light can
// reach. The lighting draw that follows then scales its light by that alpha.
bool drawLocalLight(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, Falloff falloff)
{
    ComPtr<ID3D11BlendState> gameBlend;
    float blendFactor[4]{};
    UINT sampleMask{};
    context->OMGetBlendState(gameBlend.GetAddressOf(), blendFactor, &sampleMask);
    if (!gameBlend) return false;

    D3D11_BLEND_DESC blend{};
    gameBlend->GetDesc(&blend);
    const bool marking = blend.RenderTarget[0].RenderTargetWriteMask == 0;
    const auto [scene, light] = passConstants(context);
    if (!scene || !light)
    {
        std::lock_guard lock(resourceMutex);
        logOnce("a light without its constants keeps the game's shadows.");
        return false;
    }

    const std::array position{light[LightPosition], light[LightPosition + 1], light[LightPosition + 2]};
    if (!marking)
    {
        if (!takeMarked(context, position)) return false;

        std::lock_guard lock(resourceMutex);
        auto* shadowed = shadowedBlend(gameBlend.Get());
        if (!shadowed)
        {
            logOnce("a light with another blend than (one, one) keeps the game's shadows.");
            return false;
        }

        context->OMSetBlendState(shadowed, blendFactor, sampleMask);
        context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
        context->OMSetBlendState(gameBlend.Get(), blendFactor, sampleMask);

        return true;
    }

    ComPtr<ID3D11ShaderResourceView> depth, specular;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    context->PSGetShaderResources(SpecularSlot, 1, specular.GetAddressOf());
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    if (!depth)
    {
        logOnce("a light without its depth keeps the game's shadows.");
        return false;
    }

    // A light that cannot reach the screen keeps the game's draws.
    const float range = rangeOf(light, falloff);
    const auto bounds = boundsOf(scene, position, range, sizeOf(depth.Get()));
    if (!bounds) return false;

    float point[4]{position[0], position[1], position[2], 1};
    if (std::abs(point[2]) < MinLightDepth) point[2] = std::copysign(MinLightDepth, point[2]);
    performance::Timed timed(context, performance::Pass::LampTraces);
    const auto dispatches = trace(context, depth.Get(), specular.Get(), project(scene, point), bounds, lightShadow);
    timed.end();
    if (dispatches.DispatchCount == 0) return false;

    if (auto& once = tracedLightOnce[static_cast<std::size_t>(falloff)]; !once)
    {
        once = true;
        log("Shadows: first local light trace (" + std::string(falloff == Falloff::Quartic ? "SSLighting2" : "Lighting_NA") + "), light at "
            + std::to_string(position[0]) + ", " + std::to_string(position[1]) + ", " + std::to_string(position[2]) + " (view space), range "
            + std::to_string(range) + " m, pixels " + std::to_string(bounds->min[0]) + ", " + std::to_string(bounds->min[1]) + " to "
            + std::to_string(bounds->max[0]) + ", " + std::to_string(bounds->max[1]) + ", depth " + std::to_string(bounds->depthMin) + " to "
            + std::to_string(bounds->depthMax) + ", " + describe(dispatches) + ".");
    }

    ComPtr<ID3D11PixelShader> gameShader;
    ComPtr<ID3D11ShaderResourceView> gameView;
    ComPtr<ID3D11Buffer> gameOptions;
    ComPtr<ID3D11DepthStencilState> gameDepth;
    UINT stencilReference{};
    ComPtr<ID3D11RasterizerState> gameRasterizer;
    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetShaderResources(ShadowSlot, 1, gameView.GetAddressOf());
    context->PSGetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
    context->OMGetDepthStencilState(gameDepth.GetAddressOf(), &stencilReference);
    context->RSGetState(gameRasterizer.GetAddressOf());

    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    timed.next(performance::Pass::LampShadows);
    context->PSSetShader(lightShadowShader.Get(), nullptr, 0);
    context->PSSetShaderResources(ShadowSlot, 1, lightShadow.view.GetAddressOf());
    context->PSSetConstantBuffers(OptionsSlot, 1, parameterBuffer.GetAddressOf());
    context->OMSetBlendState(alphaOnly.Get(), blendFactor, sampleMask);
    context->OMSetDepthStencilState(noDepth.Get(), 0);
    context->RSSetState(bothFaces.Get());
    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    timed.end();
    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetShaderResources(ShadowSlot, 1, gameView.GetAddressOf());
    context->PSSetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
    context->OMSetBlendState(gameBlend.Get(), blendFactor, sampleMask);
    context->OMSetDepthStencilState(gameDepth.Get(), stencilReference);
    context->RSSetState(gameRasterizer.Get());
    noteMarked(context, position);

    return true;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    bool wantedOn;
    {
        std::lock_guard lock(optionsMutex);
        wantedOn = wanted.soft || wanted.screenSpaceSun || wanted.screenSpaceLights || wanted.rayTraced;
    }

    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (wantedOn) ensurePipeline();
}

void detach() noexcept
{
    std::lock_guard lock(resourceMutex);
    traceShader.Reset();
    for (auto& replacement : replacements) replacement.Reset();
    lightShadowShader.Reset();
    borderSampler.Reset();
    parameterBuffer.Reset();
    optionsBuffer.Reset();
    alphaOnly.Reset();
    noDepth.Reset();
    bothFaces.Reset();
    shadowedBlends.clear();
    sunShadow = {};
    lightShadow = {};
    device.Reset();
    pipelineFailed = false;
    {
        std::lock_guard tracedLock(tracedMutex);
        traced.clear();
        marked.clear();
    }

    {
        std::unique_lock lightLock(lightShaderMutex);
        lightShaders.clear();
        lightShaderOwners.clear();
    }

    std::lock_guard shaderLock(shaderMutex);
    for (std::size_t sun = 0; sun < sunShaders.size(); ++sun)
        for (std::size_t i = 0; i < CopiesPerShader; ++i)
        {
            sunShaders[sun][i] = nullptr;
            shaderOwners[sun][i].Reset();
        }
}

void configure(const Options& options) noexcept
{
    {
        std::lock_guard lock(optionsMutex);
        wanted = options;
    }

    if (!options.soft && !options.screenSpaceSun && !options.screenSpaceLights && !options.rayTraced) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

bool failed() noexcept
{
    return pipelineFailed;
}

void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    const auto text = toHex(*checksum);
    for (std::size_t sun = 0; sun < std::size(SunChecksums); ++sun)
    {
        if (text != SunChecksums[sun]) continue;

        std::lock_guard lock(shaderMutex);
        for (std::size_t i = 0; i < CopiesPerShader; ++i)
        {
            if (sunShaders[sun][i].load()) continue;

            shaderOwners[sun][i] = shader;
            sunShaders[sun][i] = shader;
            log(std::string("Identified the sun light pass (") + SunNames[sun] + ").");
            return;
        }
    }

    const auto light = std::find_if(std::begin(LocalLightShaders), std::end(LocalLightShaders), [&](const auto& known) { return text == known.checksum; });
    if (light == std::end(LocalLightShaders)) return;

    try
    {
        std::unique_lock lock(lightShaderMutex);
        lightShaders.emplace(shader, light->falloff);
        lightShaderOwners.emplace_back(shader);
    }
    catch (...)
    {
    }
}

void bufferMapped(ID3D11Resource* buffer, void* data) noexcept
{
    if (!tracing.load(std::memory_order_relaxed) || !data) return;

    D3D11_RESOURCE_DIMENSION dimension{};
    buffer->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return;

    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(buffer)->GetDesc(&desc);
    if ((desc.ByteWidth != LightConstantsSize && desc.ByteWidth != SceneConstantsSize) || !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;

    mappedBuffer = buffer;
    mappedData = data;
    mappedSize = desc.ByteWidth;
}

void bufferUnmapped(ID3D11Resource* buffer) noexcept
{
    if (!mappedBuffer || buffer != mappedBuffer) return;

    try
    {
        auto& contents = written[buffer];
        contents.resize(mappedSize / sizeof(float));
        std::memcpy(contents.data(), mappedData, mappedSize);
    }
    catch (...)
    {
    }

    mappedBuffer = nullptr;
    mappedData = nullptr;
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!active.load(std::memory_order_relaxed) || call.instances != 1 || (!call.indexed && call.count != 3)) return false;

    try
    {
        ComPtr<ID3D11PixelShader> gameShader;
        context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
        const auto sun = call.indexed ? Sun::None : sunOf(gameShader.Get());
        const auto falloff = call.indexed ? falloffOf(gameShader.Get()) : std::optional<Falloff>{};
        if (sun == Sun::None && !falloff) return false;

        Options options;
        {
            std::lock_guard lock(optionsMutex);
            options = current;
        }

        if (sun != Sun::None) return drawSun(context, call, sun, gameShader.Get(), options);

        return options.screenSpaceLights && drawLocalLight(context, call, *falloff);
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("a light pass kept the game's shadows: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void contextFinished(ID3D11DeviceContext* context) noexcept
{
    std::lock_guard lock(tracedMutex);
    traced.erase(context);
    marked.erase(context);
}

void present() noexcept
{
    {
        std::lock_guard lock(tracedMutex);
        traced.clear();
        marked.clear();
    }

    frame.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(optionsMutex);
    current = wanted;
    active = (current.soft || current.screenSpaceSun || current.screenSpaceLights || current.rayTraced) && !pipelineFailed;
    tracing = active && (current.screenSpaceSun || current.screenSpaceLights);
}
}
