#include "passes/indirect_light_pass.hpp"

#include "d3d11_helpers.hpp"
#include "features/ambient_occlusion/gtvbao.hpp"
#include "features/global_illumination/light_combine.hpp"
#include "features/indirect_light/ssilvb.hpp"
#include "features/ray_traced_lights/local_lights.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/indirect_light_pass_shaders.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/visibility_bitmask.hpp"
#include "shader_bytecode.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phantom_fx::indirect_light_pass
{
namespace
{
using Microsoft::WRL::ComPtr;

// The game's pixel shaders the effects work with (docs/DEVELOPMENT.md, "Ambient occlusion").
enum class Role : std::uint8_t
{
    SkyLight,      // Binds the G-buffer normals, right before the upscale in the same command list
    Upscale,       // Writes the indirect light into the diffuse light buffer
    Resolve,       // Multiplies all light by the game's occlusion
    GameOcclusion, // The game's SSAO, whose result only the resolve reads
    Sun,           // Binds the G-buffer's specular target, which holds the material
    SunShadowMap,  // The same, inside the shadow map's range
    SkySpecular,   // Indoors, where no sun shines, the sky's specular alone in the sun pass's place
};

struct GameShader
{
    Role role;
    const char* name;
    const char* checksum;
};

constexpr GameShader Shaders[]{
    {Role::SkyLight, "SSLighting2_SH_SkyLight_Encode", "ac49de83da6ebb80336de04c4ef7d022"},
    {Role::Upscale, "BilateralUpscale2x2", "1314ac1eb31bb06b4e972fdc2c24ccbf"},
    {Role::Resolve, "DR_VolFog_TppTonemap", "00a609370c2e8b47a645263f91f6d3e8"},
    {Role::Resolve, "DR_VolFog_TppTonemap_MD", "79c001e295254d8a749e41d1d7e49de7"},
    {Role::Resolve, "DR_TonemapRainFilter", "9a4abc815afc5f121572d695f07af5af"},
    {Role::Resolve, "DR_TonemapRainFilter_NoIR", "47f52852eaccaa83c25745ce790a212c"},
    {Role::Resolve, "DeferredRendering", "df87979884e90877ee0337c13e01664f"},
    {Role::Resolve, "DeferredRendering_VolFog", "322890810465ee05f4895bc69e80a187"},
    {Role::Resolve, "DeferredRenderingTone", "47256d38f45b59b6c46cf32bf29564f0"},
    {Role::Resolve, "DeferredRenderingTone_GVFog", "86a042757928c965fff1c016647c4dfa"},
    {Role::Resolve, "DeferredRenderingFilmic", "586e33bca62435478f23154da9e25187"},
    {Role::Resolve, "DeferredRenderingFilmic_VolFog", "504b446c3e795d75e6adcd6753c11d12"},
    {Role::GameOcclusion, "SSAOLineIntegrals", "1e716301e9c644c4d59c620e9b84f7de"},
    {Role::GameOcclusion, "AmbientObscuranceSSAO", "9f963f50539aafc2ee77b888c56d3a0f"},
    {Role::SunShadowMap, "SunLight_DL_CS_ES", "56ed4db892a7940f038146eeb0df0d30"},
    {Role::Sun, "SunLight_DL_ES", "92726e8d231c8ed85f0db90400ef2258"},
    {Role::SunShadowMap, "SunLight_DL_CS", "7e5c2fc99c9d84ddd32df4e789e7082a"}, // In rain
    {Role::Sun, "SunLight_DL", "853f56a63e5bc66e6ee301c85ebfeaa8"},    // In rain
    {Role::SkySpecular, "SunLight_ES", "367e8f6128befd48f2840ab3d6ed19b4"}, // Indoors
};

constexpr UINT NormalSlot = 0;   // The sky light's g_NormalTexture
constexpr UINT SpecularSlot = 1; // The sun's g_SpecularTexture
constexpr UINT DepthSlot = 2;    // The upscale's inDepthBuffer
constexpr UINT SceneSlot = 2;    // cPSScene, in the upscale and the resolves
// Every resolve's inAlbedo, inLightDiffuse, inDepth and inOcclusion.
constexpr UINT AlbedoSlot = 8, DiffuseSlot = 9, ResolveDepthSlot = 11, OcclusionSlot = 13;

// The effects that change these passes, by the effect plan, and the diagnostics' views of them.
struct Composition
{
    bool occlusion{}, light{}, globalIllumination{};
    // The ray-traced scene runs, for the global illumination or the ray-traced shadows or lights: the
    // passes tell it what it needs (ray_tracing::noteNormals and the others).
    bool rayTracing{};
    bool showOcclusion{}, showLight{}, showGlobalIllumination{};
};

FrameLatch<Composition> latch;
std::atomic<bool> active{}, pipelineFailed{};

// Identified game shaders, by the object the game draws with. Holding them keeps their addresses
// from being reused.
std::shared_mutex shaderMutex;
std::unordered_map<IUnknown*, Role> roles;
std::vector<ComPtr<IUnknown>> shaderOwners;

// The normals each context's sky light pass bound, for the upscale it records next. The resolve is
// in another command list; it takes the G-buffer normals any sky light pass bound last. The sun pass
// comes after the upscale, so the march takes the specular target from an earlier frame's: the
// same texture while the render size stays.
std::mutex gBufferMutex;
std::unordered_map<ID3D11DeviceContext*, ComPtr<ID3D11ShaderResourceView>> normals;
ComPtr<ID3D11ShaderResourceView> lastNormals, lastSpecular;

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> showOcclusionShader, showLightShader;
ComPtr<ID3D11ShaderResourceView> whiteView; // 1x1, the resolve's occlusion while an effect replaces the game's
// The game's diffuse light with SSILVB's or the global illumination's added, read by the resolve in
// its place.
visibility_bitmask::Target diffuseWithLight;

// Compiles the shaders and makes the fixed resources once; call with resourceMutex held. Returns
// false when they are unavailable.
bool ensurePipeline()
{
    if (whiteView) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        const D3D_SHADER_MACRO occlusionView[]{{"LIGHT", "0"}, {}}, lightView[]{{"LIGHT", "1"}, {}};
        showOcclusionShader = pixelShader(device.Get(), shaders::Show, "gtvbao-show", occlusionView);
        showLightShader = pixelShader(device.Get(), shaders::Show, "ssilvb-show", lightView);

        D3D11_TEXTURE2D_DESC white{};
        white.Width = white.Height = white.MipLevels = white.ArraySize = 1;
        white.Format = DXGI_FORMAT_R8_UNORM;
        white.SampleDesc.Count = 1;
        white.Usage = D3D11_USAGE_IMMUTABLE;
        white.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        constexpr std::uint8_t unoccluded = 255;
        const D3D11_SUBRESOURCE_DATA data{&unoccluded, 1, 1};
        ComPtr<ID3D11Texture2D> whiteTexture;
        check(device->CreateTexture2D(&white, &data, whiteTexture.GetAddressOf()), "Creating the white occlusion");
        // Last, so it marks a complete pipeline.
        check(device->CreateShaderResourceView(whiteTexture.Get(), nullptr, whiteView.ReleaseAndGetAddressOf()), "Creating the white occlusion");
        log("Indirect light pass ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("GTVBAO, SSILVB and the global illumination's light are unavailable: ") + error.what());

        return false;
    }
}

std::optional<Role> roleOf(IUnknown* shader)
{
    if (!shader) return std::nullopt;

    std::shared_lock lock(shaderMutex);
    const auto found = roles.find(shader);

    return found != roles.end() ? std::optional(found->second) : std::nullopt;
}

void noteNormals(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> view;
    context->PSGetShaderResources(NormalSlot, 1, view.GetAddressOf());
    std::lock_guard lock(gBufferMutex);
    lastNormals = view;
    normals[context] = std::move(view);
}

void noteSpecular(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11ShaderResourceView> view;
    context->PSGetShaderResources(SpecularSlot, 1, view.GetAddressOf());
    std::lock_guard lock(gBufferMutex);
    lastSpecular = std::move(view);
}

ComPtr<ID3D11ShaderResourceView> takeNormals(ID3D11DeviceContext* context)
{
    std::lock_guard lock(gBufferMutex);
    const auto found = normals.find(context);
    if (found == normals.end()) return nullptr;

    auto view = std::move(found->second);
    normals.erase(found);

    return view;
}

ComPtr<ID3D11ShaderResourceView> latestNormals()
{
    std::lock_guard lock(gBufferMutex);
    return lastNormals;
}

// The specular target a sun pass bound last, if it has the render size; otherwise none, and the
// march treats nothing as foliage.
ComPtr<ID3D11ShaderResourceView> latestSpecular(std::pair<UINT, UINT> size)
{
    ComPtr<ID3D11ShaderResourceView> view;
    {
        std::lock_guard lock(gBufferMutex);
        view = lastSpecular;
    }

    return sizeOf(view.Get()) == size ? view : nullptr;
}

// The game's upscale, then GTVBAO's occlusion multiplied into what it wrote. False leaves the draw to the game.
bool drawUpscale(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    const auto normalView = takeNormals(context);
    ComPtr<ID3D11ShaderResourceView> depth;
    ComPtr<ID3D11Buffer> scene;
    ComPtr<ID3D11RenderTargetView> light;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    context->PSGetConstantBuffers(SceneSlot, 1, scene.GetAddressOf());
    context->OMGetRenderTargets(1, light.GetAddressOf(), nullptr);
    const auto specular = latestSpecular(sizeOf(depth.Get()));
    if (!gtvbao::compute(context, depth.Get(), normalView.Get(), specular.Get(), light.Get(), scene.Get())) return false;

    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    gtvbao::apply(context, call);

    return true;
}

// The resolve, with the diffuse light SSILVB added to while it runs, and white in place of the
// game's occlusion while GTVBAO runs; or, for the diagnostics, either effect alone. False leaves
// the draw to the game.
bool drawResolve(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const Composition& composition)
{
    const auto normalView = composition.light ? latestNormals() : nullptr;
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    bool lit{};
    if (composition.light)
    {
        ComPtr<ID3D11ShaderResourceView> depth, diffuse, albedo;
        ComPtr<ID3D11Buffer> scene;
        context->PSGetShaderResources(ResolveDepthSlot, 1, depth.GetAddressOf());
        context->PSGetShaderResources(DiffuseSlot, 1, diffuse.GetAddressOf());
        context->PSGetShaderResources(AlbedoSlot, 1, albedo.GetAddressOf());
        context->PSGetConstantBuffers(SceneSlot, 1, scene.GetAddressOf());
        const auto specular = latestSpecular(sizeOf(depth.Get()));
        lit = ssilvb::compute(context, depth.Get(), normalView.Get(), diffuse.Get(), albedo.Get(), specular.Get(), scene.Get(), diffuseWithLight);
    }

    if (!lit && !composition.occlusion) return false;

    ComPtr<ID3D11PixelShader> gameShader;
    ComPtr<ID3D11ShaderResourceView> gameDiffuse, gameOcclusion;
    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetShaderResources(DiffuseSlot, 1, gameDiffuse.GetAddressOf());
    context->PSGetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());

    auto* occlusion = composition.occlusion && composition.showOcclusion ? gtvbao::occlusion() : nullptr;
    if (lit) context->PSSetShaderResources(DiffuseSlot, 1, diffuseWithLight.view.GetAddressOf());
    if (composition.occlusion) context->PSSetShaderResources(OcclusionSlot, 1, whiteView.GetAddressOf());
    if (lit && composition.showLight) context->PSSetShader(showLightShader.Get(), nullptr, 0);
    else if (occlusion)
    {
        context->PSSetShader(showOcclusionShader.Get(), nullptr, 0);
        context->PSSetShaderResources(OcclusionSlot, 1, &occlusion);
    }

    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetShaderResources(DiffuseSlot, 1, gameDiffuse.GetAddressOf());
    context->PSSetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());

    return true;
}

// What the ray-traced scene needs from the game's passes (ray_tracing.hpp): the normals, the
// kick at the upscale, the wait at the resolve, and the sun; the first of the sun pass, the sky's specular
// pass and the resolve draws the point and spot lights too.
void noteForRayTracing(ID3D11DeviceContext* context, Role role)
{
    ComPtr<ID3D11ShaderResourceView> view;
    switch (role)
    {
    case Role::SkyLight:
        context->PSGetShaderResources(NormalSlot, 1, view.GetAddressOf());
        ray_tracing::noteNormals(context, view.Get());
        return;
    case Role::Upscale:
        context->PSGetShaderResources(DepthSlot, 1, view.GetAddressOf());
        ray_tracing::noteUpscale(context, view.Get());
        return;
    case Role::Resolve:
        context->PSGetShaderResources(AlbedoSlot, 1, view.GetAddressOf());
        ray_tracing::noteResolve(context, view.Get());
        local_lights::noteDrawPoint(context);
        return;
    case Role::Sun:
        ray_tracing::noteSun(context, false);
        local_lights::noteDrawPoint(context);
        return;
    case Role::SunShadowMap:
        ray_tracing::noteSun(context, true);
        local_lights::noteDrawPoint(context);
        return;
    case Role::SkySpecular:
        ray_tracing::noteSkySpecular(context);
        local_lights::noteDrawPoint(context);
        return;
    case Role::GameOcclusion: return;
    }
}

// The resolve while the global illumination runs: the diffuse light with the traced light in place
// of the game's probe light, white in place of the game's occlusion; or, for the diagnostics, the
// global illumination's output alone. False leaves the draw to the game.
bool drawResolveForGlobal(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, const Composition& composition)
{
    if (!ray_tracing::lightRuns()) return false;

    ComPtr<ID3D11ShaderResourceView> depth, diffuse, gameOcclusion;
    ComPtr<ID3D11PixelShader> gameShader;
    context->PSGetShaderResources(ResolveDepthSlot, 1, depth.GetAddressOf());
    context->PSGetShaderResources(DiffuseSlot, 1, diffuse.GetAddressOf());

    const auto size = sizeOf(depth.Get());
    auto* light = ray_tracing::light(size.first, size.second);
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;
    if (!light_combine::combine(context, diffuse.Get(), light, size, diffuseWithLight)) return false;

    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());
    if (composition.showGlobalIllumination)
    {
        context->PSSetShader(showLightShader.Get(), nullptr, 0);
        context->PSSetShaderResources(DiffuseSlot, 1, &light);
    }
    else context->PSSetShaderResources(DiffuseSlot, 1, diffuseWithLight.view.GetAddressOf());

    context->PSSetShaderResources(OcclusionSlot, 1, whiteView.GetAddressOf());
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetShaderResources(DiffuseSlot, 1, diffuse.GetAddressOf());
    context->PSSetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());

    return true;
}

// A smaller camera's resolve while the global illumination runs, as the intro mirror's: the rays trace
// the main view only, so the game's own light, with white in place of its occlusion, which is skipped.
bool drawSmallerViewResolve(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    if (!ray_tracing::lightRuns()) return false;

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    ComPtr<ID3D11ShaderResourceView> gameOcclusion;
    context->PSGetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());
    context->PSSetShaderResources(OcclusionSlot, 1, whiteView.GetAddressOf());
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->PSSetShaderResources(OcclusionSlot, 1, gameOcclusion.GetAddressOf());

    return true;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    const auto wanted = latch.latest();
    const bool enable = wanted.occlusion || wanted.light || wanted.globalIllumination;

    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (enable) ensurePipeline();
}

void detach() noexcept
{
    active = false;
    {
        std::lock_guard lock(resourceMutex);
        showOcclusionShader.Reset();
        showLightShader.Reset();
        whiteView.Reset();
        diffuseWithLight = {};
        device.Reset();
        pipelineFailed = false;
    }

    {
        std::lock_guard lock(gBufferMutex);
        normals.clear();
        lastNormals.Reset();
        lastSpecular.Reset();
    }

    std::unique_lock lock(shaderMutex);
    roles.clear();
    shaderOwners.clear();
}

void configure(const Configuration& configuration) noexcept
{
    const auto& diagnostics = configuration.diagnostics;
    const Composition composition{
        .occlusion = configuration.effects.runs(Effect::AmbientOcclusion),
        .light = configuration.effects.runs(Effect::IndirectLight),
        .globalIllumination = configuration.effects.runs(Effect::GlobalIllumination),
        .rayTracing = configuration.effects.tracesScene(),
        .showOcclusion = diagnostics.showAmbientOcclusion,
        .showLight = diagnostics.showIndirectLight,
        .showGlobalIllumination = diagnostics.showGlobalIllumination || diagnostics.showTracedScene,
    };

    latch.set(composition);

    if (!composition.occlusion && !composition.light && !composition.globalIllumination) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

bool failed() noexcept
{
    return pipelineFailed;
}

ComPtr<ID3D11ShaderResourceView> specularTarget(UINT width, UINT height) noexcept
{
    try
    {
        return latestSpecular({width, height});
    }
    catch (...)
    {
        return nullptr;
    }
}

void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    const auto text = toHex(*checksum);
    for (const auto& known : Shaders)
    {
        if (text != known.checksum) continue;

        try
        {
            std::unique_lock lock(shaderMutex);
            roles[shader] = known.role;
            shaderOwners.emplace_back(shader);
        }
        catch (...)
        {
            return;
        }

        log(std::string("Identified ") + known.name + ".");
        return;
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!active.load(std::memory_order_relaxed) || call.indexed || call.count != 3 || call.instances != 1) return false;

    try
    {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        const auto role = roleOf(shader.Get());
        if (!role) return false;

        const auto composition = latch.frame();

        // The rays trace the main view only, one kick a frame: a smaller camera's view in a corner of the
        // same targets, as the intro's mirror, keeps the game's light.
        const bool mainView = viewportFillsTarget(context);
        if (composition.rayTracing && mainView) noteForRayTracing(context, *role);
        if (composition.globalIllumination)
            switch (*role)
            {
            case Role::Upscale:
                if (mainView && ray_tracing::lightRuns()) light_combine::drawProbeLightAside(context, call);
                return false;
            case Role::Resolve: return mainView ? drawResolveForGlobal(context, call, composition) : drawSmallerViewResolve(context, call);
            case Role::GameOcclusion: return ray_tracing::lightRuns();
            case Role::Sun:
            case Role::SunShadowMap: noteSpecular(context); return false;
            case Role::SkyLight:
            case Role::SkySpecular: return false;
            }

        switch (*role)
        {
        case Role::SkyLight: noteNormals(context); return false;
        case Role::Upscale: return composition.occlusion && drawUpscale(context, call);
        case Role::Resolve: return drawResolve(context, call, composition);
        case Role::GameOcclusion: return composition.occlusion;
        case Role::Sun:
        case Role::SunShadowMap: noteSpecular(context); return false;
        case Role::SkySpecular: return false;
        }
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("GTVBAO or SSILVB: a pass kept the game's drawing: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void present() noexcept
{
    const auto current = latch.present();
    active = (current.occlusion || current.light || current.globalIllumination || current.rayTracing) && !pipelineFailed;
}
}
