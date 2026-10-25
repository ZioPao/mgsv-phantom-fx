#include "shadows.hpp"

#include "d3d11_helpers.hpp"
#include "game_lights.hpp"
#include "gi_inputs.hpp"
#include "global_illumination.hpp"
#include "log.hpp"
#include "performance.hpp"
#include "shader_bytecode.hpp"
#include "shadows_shaders.hpp"

#include <bend_sss/bend_sss_cpu.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
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
using namespace game_lights;

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

// The depth a context's recording traced the sun from since its last FinishCommandList: both parts
// of the sun pass read the same shadow. And the light whose shadow a context's last stencil-marking
// draw put in the light targets' alpha, by its position, for the lighting draw that follows.
std::mutex tracedMutex;
std::unordered_map<ID3D11DeviceContext*, ID3D11ShaderResourceView*> traced;
std::unordered_map<ID3D11DeviceContext*, std::array<float, 3>> marked;

// A screen-space shadow, 1 where the light reaches, at the depth buffer's size.
using ShadowTarget = ComputeTexture;

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

// Compiles the shaders once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (traceShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        traceShader = computeShader(device.Get(), shaders::BendShadow, "bend-screen-space-shadows");

        for (std::size_t index = 0; index < replacements.size(); ++index)
        {
            const auto sun = static_cast<Sun>(index);
            const D3D_SHADER_MACRO defines[]{{"SHADOW_MAP", shadowMapped(sun) ? "1" : "0"}, {"SKY_REFLECTION", reflectsSky(sun) ? "1" : "0"}, {}};
            replacements[index] = pixelShader(device.Get(), shaders::SunLight, "sun-light", defines);
        }

        lightShadowShader = pixelShader(device.Get(), shaders::LightShadow, "light-shadow");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER; // Border colour 0: the far plane
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, borderSampler.ReleaseAndGetAddressOf()), "Creating the shadow trace sampler");

        parameterBuffer = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the shadow trace constants");
        optionsBuffer = makeConstantBuffer(device.Get(), sizeof(ShaderOptions), "Creating the sun shadow options");

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

    // Command lists that still use the previous texture hold their own references.
    target = makeComputeTexture(device.Get(), width, height, DXGI_FORMAT_R8_UNORM, "Creating the screen-space shadow");
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

// What this thread wrote to the light pass's cPSScene (b2) and cPSLight (b3), each at its full size.
struct PassConstants
{
    std::array<float, SceneConstantsSize / 4> scene{};
    std::array<float, LightConstantsSize / 4> light{};
};

// None where unknown.
std::optional<PassConstants> passConstants(ID3D11DeviceContext* context)
{
    std::array<ID3D11Buffer*, 2> constants{};
    context->PSGetConstantBuffers(2, 2, constants.data());
    PassConstants written;
    const bool known = gi_inputs::writtenExactly(constants[0], written.scene) && gi_inputs::writtenExactly(constants[1], written.light);
    for (auto* buffer : constants)
        if (buffer) buffer->Release();
    if (!known) return std::nullopt;

    return written;
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

// The width and height of the depth buffer `view` shows; throws when it is not a 2D texture, as the
// trace needs one.
std::array<int, 2> depthSize(ID3D11ShaderResourceView* view)
{
    const auto [width, height] = sizeOf(view);
    if (!width) throw std::runtime_error("Reading the depth buffer failed: it is not a 2D texture.");

    return {static_cast<int>(width), static_cast<int>(height)};
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

    // Pixels as Bend places the light.
    const auto rect = sphereOnScreen(scene, center, range, {static_cast<float>(size[0]), static_cast<float>(size[1])});
    if (!rect) return std::nullopt;

    for (int axis = 0; axis < 2; ++axis)
    {
        bounds.min[axis] = rect->min[axis];
        bounds.max[axis] = rect->max[axis];
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
    auto size = depthSize(depth);
    ensureShadowTarget(target, static_cast<UINT>(size[0]), static_cast<UINT>(size[1]));

    // Bend's default depth bounds skip the sky.
    const auto covered = bounds.value_or(TraceBounds{{0, 0}, {size[0], size[1]}, 0, 1});
    int boundsMin[2]{covered.min[0], covered.min[1]}, boundsMax[2]{covered.max[0], covered.max[1]};
    const auto dispatches = Bend::BuildDispatchList(lightProjection.data(), size.data(), boundsMin, boundsMax, false, WaveSize);

    const SavedComputeState saved(context, {.buffers = 1, .views = 2, .accesses = 1, .samplers = 1});

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
        if (FAILED(write(context, parameterBuffer.Get(), parameters))) break;

        context->Dispatch(static_cast<UINT>(dispatch.WaveCount[0]), static_cast<UINT>(dispatch.WaveCount[1]), static_cast<UINT>(dispatch.WaveCount[2]));
    }

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
    constexpr float DegreesToRadians = 3.14159265f / 180;
    const ShaderOptions values{options.soft || rayTraced ? 1.0f : 0.0f, std::tan(options.sunSize * 0.5f * DegreesToRadians),
        options.turnPattern ? static_cast<float>(frame.load(std::memory_order_relaxed) % PatternTurns) : 0.0f, options.screenSpaceSun ? 1.0f : 0.0f,
        rayTraced ? 1.0f : 0.0f, {}};
    check(write(context, optionsBuffer.Get(), values), "Writing the sun shadow options");
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
        const auto size = depthSize(depth.Get());
        rayTraced = global_illumination::sunShadow(static_cast<UINT>(size[0]), static_cast<UINT>(size[1]));
    }

    // Beyond the shadow range there is no shadow map to soften.
    if (!options.screenSpaceSun && !(options.soft && shadowMapped(sun)) && !rayTraced) return false;

    const auto constants = passConstants(context);

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    if (options.screenSpaceSun)
    {
        if (!depth || !constants)
        {
            logOnce("Shadows: a sun pass without its depth or constants keeps the game's shadows.");
            return false;
        }

        if (!tracedFrom(context, depth.Get()))
        {
            const performance::Timed timed(context, performance::Pass::SunTrace);
            const float* direction = &constants->light[LightDirection];
            const auto lightProjection = project(constants->scene.data(), {direction[0], direction[1], direction[2], 0});
            const auto where = describe(trace(context, depth.Get(), specular.Get(), lightProjection, {}, sunShadow));
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

    // The rays' shadow alone, unless the shadow map's joins it: everything lit as beyond the shadow range.
    if (rayTraced && !options.withShadowMap) sun = reflectsSky(sun) ? Sun::Far : Sun::RainFar;

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
    if (options.soft) logOnce("Shadows: the sun pass draws with soft shadows.");
    if (rayTraced) logOnce("Shadows: the sun pass draws with ray-traced shadows.");

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
    const auto constants = passConstants(context);
    if (!constants)
    {
        std::lock_guard lock(resourceMutex);
        logOnce("Shadows: a light without its constants keeps the game's shadows.");
        return false;
    }

    const float* scene = constants->scene.data();
    const float* light = constants->light.data();

    const std::array position{light[LightPosition], light[LightPosition + 1], light[LightPosition + 2]};
    if (!marking)
    {
        if (!takeMarked(context, position)) return false;

        std::lock_guard lock(resourceMutex);
        auto* shadowed = shadowedBlend(gameBlend.Get());
        if (!shadowed)
        {
            logOnce("Shadows: a light with another blend than (one, one) keeps the game's shadows.");
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
        logOnce("Shadows: a light without its depth keeps the game's shadows.");
        return false;
    }

    // A light that cannot reach the screen keeps the game's draws.
    const float range = eitherShapeRangeOf(light, falloff);
    const auto bounds = boundsOf(scene, position, range, depthSize(depth.Get()));
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

    const auto* light = findGameLight(text);
    if (!light) return;

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
        logOnce(std::string("Shadows: a light pass kept the game's shadows: ") + error.what());
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
    gi_inputs::keepLightPassConstants(active && (current.screenSpaceSun || current.screenSpaceLights));
}
}
