#include "local_lights.hpp"

#include "gi_inputs.hpp"
#include "global_illumination.hpp"
#include "log.hpp"
#include "performance.hpp"
#include "shader_bytecode.hpp"
#include "shaders/gi_shared.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace phantom_fx::local_lights
{
namespace
{
using Microsoft::WRL::ComPtr;

enum class Shape : std::uint8_t { Point, Spot };
// How a light fades with the distance d (docs/DEVELOPMENT.md, "Screen-space shadows"):
// - Quartic (SSLighting2_*): 1 / d^2 - d^2 c6.w, zero at d = c6.w^(-1/4).
// - Sixteenth (Lighting_*_NA): (1 - d^16 c6.z) / d^2, zero at d = c6.z^(-1/16).
// A point light adds c6.x to its distance first.
enum class Falloff : std::uint8_t { Quartic, Sixteenth };
// The game's shader for each kind of light with specular and without a shadow map, which draws the
// lights of that kind again.
enum class Twin : std::uint8_t { PointNA, SpotNA, MaskedSpotNA, Point2, Spot2, MaskedSpot2, Count };

constexpr const char* TwinChecksums[]{
    "32bdd328651cf4f11ebcc47c0b65dfde", // Lighting_Point_NA_NShdw
    "dddab560572d7352181cd83f00f6dd63", // Lighting_Spot_NA_NShdw
    "dcc81c5f2c97e20003a27a9e7db54376", // Lighting_Spot_NA_Mask_NShdw
    "4eb687f92330ee7b11ccf8e32950e4a9", // SSLighting2_Point_NoShadow
    "74063ef2a991625184fdf283ba58c43f", // SSLighting2_Spot_NoShadow
    "2d985165a56e0622d42f086a28c305d0", // SSLighting2_Spot_Mask_NShdw
};
static_assert(std::size(TwinChecksums) == static_cast<std::size_t>(Twin::Count));

struct GameLight
{
    const char* checksum;
    Shape shape;
    Falloff falloff;
    Twin twin;
    bool specular;       // The game's shader adds specular; without, its light's specular scale is 0
    bool flashlight{};   // The weapon flashlights' shader: every light it drew in the captures was one
};

// Every point and spot light shader of the game's light pass.
constexpr GameLight GameLights[]{
    {"e61cc726c5c877555261f03dbc4aa967", Shape::Point, Falloff::Sixteenth, Twin::PointNA, true},       // Lighting_Point_NA
    {"32bdd328651cf4f11ebcc47c0b65dfde", Shape::Point, Falloff::Sixteenth, Twin::PointNA, true},       // Lighting_Point_NA_NShdw
    {"f037f71a002e42f0cb07ad2768f7b8ac", Shape::Point, Falloff::Sixteenth, Twin::PointNA, false},      // Lighting_Point_NA_NoSpec
    {"259fff504d9caef06a467ff6ce9ee1bf", Shape::Point, Falloff::Sixteenth, Twin::PointNA, false},      // Lighting_Point_NA_NoSpec_NShdw
    {"57b7365fcfb89c3f91fffcebf1a6b1e5", Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, true},         // Lighting_Spot_NA
    {"dddab560572d7352181cd83f00f6dd63", Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, true},         // Lighting_Spot_NA_NShdw
    {"7d2ed256fcff999707444bb115321c7f", Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, false},        // Lighting_Spot_NA_NoSpec
    {"91bc5dd518df1368e60d53c5f065e9f4", Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, false},        // Lighting_Spot_NA_NoSpec_NShdw
    {"d53793289e8c2824800876d8525aab2c", Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, true},   // Lighting_Spot_NA_Mask
    {"dcc81c5f2c97e20003a27a9e7db54376", Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, true},   // Lighting_Spot_NA_Mask_NShdw
    {"a435549d3334a5715c165a68083ea66e", Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, false},  // Lighting_Spot_NA_Mask_NSpec
    {"fd570742642f09e699d193ffb02bf0df", Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, false, true}, // Lighting_Spot_NA_Mask_NSpec_NShdw (flashlights)
    {"afbc12167037e4e0d653da184c7e3e82", Shape::Point, Falloff::Quartic, Twin::Point2, true},          // SSLighting2_Point
    {"4eb687f92330ee7b11ccf8e32950e4a9", Shape::Point, Falloff::Quartic, Twin::Point2, true},          // SSLighting2_Point_NoShadow
    {"a1c56a0978c706222ebeacbf6fc9ebe4", Shape::Point, Falloff::Quartic, Twin::Point2, false},         // SSLighting2_Point_NoSpec
    {"e9cd14a10e3f1df895cb6799c232f99e", Shape::Point, Falloff::Quartic, Twin::Point2, false},         // SSLighting2_Point_NoSpec_NoShadow
    {"ac0947110cb0f17f4ffe930a6a11617d", Shape::Spot, Falloff::Quartic, Twin::Spot2, true},            // SSLighting2_Spot
    {"74063ef2a991625184fdf283ba58c43f", Shape::Spot, Falloff::Quartic, Twin::Spot2, true},            // SSLighting2_Spot_NoShadow
    {"cdc559ec2ca4d52a54697cf5ca023667", Shape::Spot, Falloff::Quartic, Twin::Spot2, false},           // SSLighting2_Spot_NoSpec
    {"a823ffb58661c508e6270227d4910c91", Shape::Spot, Falloff::Quartic, Twin::Spot2, false},           // SSLighting2_Spot_NoSpec_NoShadow
    {"9ead0f83217db6af698f860fd3233441", Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, true},      // SSLighting2_Spot_Mask
    {"2d985165a56e0622d42f086a28c305d0", Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, true},      // SSLighting2_Spot_Mask_NShdw
    {"fa6ad11cfc35a2281a22b92c1a2aa6db", Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, false},     // SSLighting2_Spot_Mask_NSpec
    {"74872741b699fdefa13ae71349f41ec9", Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, false},     // SSLighting2_Spot_Mask_NSpec_NShdw
};

// What the light shaders read: t0 to t10 (the G-buffer, the mask at t4, the specular tables at t8 and
// t9, the material table at t10), s8 to s11, and cPSScene, cPSLight and cPSMaterial at b2 to b4.
constexpr UINT ResourceSlots = 11, FirstSampler = 8, SamplerCount = 4, FirstConstants = 2;
constexpr UINT SceneConstants = 120, LightConstants = 44, MaterialConstants = 32; // Floats

// cPSScene: c8 to c10 take the light passes' view space to the world, c20 is the eye, c21 the
// projection's parameters (1 / P00, 1 / P11, B, A), c28 the jitter in NDC.
constexpr std::size_t ViewToWorld = 8 * 4, Eye = 20 * 4, ProjectionParam = 21 * 4, CenterOffset = 28 * 4;
// cPSLight, view space: c3 the light, c4 its colour and size, c5 the way a spot points, c6 its falloff,
// c7 a spot's cone (x the cosine past which it cuts its light off, y 1 / its penumbra, z the exponent),
// c8.x its specular's scale. cPSMaterial c7: x a point's least distance, w the most a spot's falloff gives.
constexpr std::size_t LightPosition = 3 * 4, LightColor = 4 * 4, LightSize = 4 * 4 + 3, LightAxis = 5 * 4, LightFalloff = 6 * 4,
                      LightCone = 7 * 4, LightUmbra = 7 * 4, SpecularScale = 8 * 4;
constexpr std::size_t LeastDistance = 7 * 4, MaxFalloff = 7 * 4 + 3;

// The game's lights with specular have a scale of 1; it is 0 for those without, as its flashlights.
constexpr float AddedSpecular = 1.0f;
// How far a light reaches when its falloff does not end, metres.
constexpr float UnendingRange = 1000.0f;
// A light sits inside its lamp's model: a ray ends this far short of it, or the light's size if larger.
constexpr float LightClearance = 0.1f;
// The radius a light's shadow rays spread over where the game gives it no size (most of its lights):
// a bulb's, so its shadows soften with distance as a real lamp's do.
constexpr float BulbRadius = 0.03f;
// GILocalLight::axis.w of a point light: below any cosine, so every way is in its "cone".
constexpr float PointCone = -2.0f;

// The full-screen triangle in the light passes' vertex shaders' form (Lighting_*_vs): the clip-space
// position twice, then where it samples the light targets, from the viewport within them.
constexpr char FullScreenSource[] = R"(
cbuffer LightPass : register(b0)
{
    float4 ViewToUv; // xy: the viewport's size over the targets'; zw: its corner over the targets'
    uint4 Shadow;    // y: 1 when the light was traced, so the share holds its shadow
};

void main(uint id : SV_VertexID, out float4 position : SV_Position, out float4 clip : TEXCOORD0, out float4 sampling : TEXCOORD1)
{
    float2 xy = float2(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0);
    position = float4(xy, 0.0, 1.0);
    clip = position;
    sampling = float4((xy * float2(0.5, -0.5) + 0.5) * ViewToUv.xy + ViewToUv.zw, 0.0, 1.0);
}
)";

// The light's shadow into both light targets' alpha, for the light's blend to scale it by.
constexpr char ShadowSource[] = R"(
cbuffer LightPass : register(b0)
{
    float4 ViewToUv;
    uint4 Shadow;
};

Texture2D<float> Share : register(t0); // The share of the traced lights' light that reaches

void main(float4 position : SV_Position, out float4 diffuse : SV_Target0, out float4 specular : SV_Target1)
{
    float lit = Shadow.y ? Share.Load(int3(position.xy, 0)) : 1.0;
    diffuse = specular = float4(0.0, 0.0, 0.0, lit);
}
)";

struct PassConstants
{
    float viewToUv[4];
    std::uint32_t shadow[4];
};

// A light's lighting draw as recorded, with what drawing it again on the immediate context needs.
struct Light
{
    const GameLight* game{};
    std::array<float, SceneConstants> scene{};
    std::array<float, LightConstants> light{};
    std::array<float, MaterialConstants> material{};
    std::array<ComPtr<ID3D11ShaderResourceView>, ResourceSlots> resources;
    std::array<ComPtr<ID3D11SamplerState>, SamplerCount> samplers;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> targets;
    D3D11_VIEWPORT viewport{};
    bool traced{}; // Given to the rays at the frame's kick: the share holds its shadow
};

// What a deferred context, then its command list, recorded: its lights, and whether it has a draw point
// (noteDrawPoint).
struct Notes
{
    std::vector<Light> lights;
    bool drawPoint{};
    std::uint32_t presents{}; // Since it was recorded: notes no list ran go after a few
};

std::mutex optionsMutex;
Options wanted, current;
// On with the pipeline and a twin of the game's: the draw points and the lists are followed. Active once a
// frame also had a draw point, whose list draws them again: the game's lights are taken over. Menus and
// loading screens, without one, keep the game's.
std::atomic<bool> enabled{}, active{};
std::atomic<float> flashlightBrightness{1};

// The game's light shaders by the object it draws with, looked up on every indexed draw, and the twins
// that draw them again. Holding them keeps their addresses from being reused.
std::shared_mutex shaderMutex;
std::unordered_map<IUnknown*, const GameLight*> gameShaders;
std::vector<ComPtr<IUnknown>> shaderOwners;
std::array<ComPtr<ID3D11PixelShader>, static_cast<std::size_t>(Twin::Count)> twins;

std::mutex notesMutex;
std::unordered_map<std::uint64_t, Notes> byContext; // While recorded
std::unordered_map<std::uint64_t, Notes> byList;    // Once finished

// ---- The immediate context's side, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ID3D11DeviceContext* immediate{};
ComPtr<ID3D11VertexShader> fullScreenShader;
ComPtr<ID3D11PixelShader> shadowShader;
ComPtr<ID3D11BlendState> alphaOnly, shadowed;
ComPtr<ID3D11RasterizerState> scissored;
ComPtr<ID3D11DepthStencilState> noDepth;
ComPtr<ID3D11Buffer> passBuffer, sceneBuffer, lightBuffer, materialBuffer;
bool pipelineFailed{};
// The frame's lights from the lists run so far, and whether a list with a draw point has run.
std::vector<Light> frameLights;
bool drawPointRan{};

// Diagnostics, logged every ten seconds: lights drawn with their shadow and without it (recorded after
// the kick, or the shadow not traced), left to the next frame's camera or over the mask's bits at the
// kick, and lost (a frame ended without a draw point after them).
std::atomic<std::uint64_t> drawnShadowed{}, drawnUnshadowed{}, otherFrame{}, overLimit{}, lost{};

void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
}

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

ComPtr<ID3D11Buffer> constantBuffer(UINT size, const char* what)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()), what);

    return buffer;
}

// The shaders, states and buffers, once; call with resourceMutex held. False when unavailable.
bool ensurePipeline()
{
    if (fullScreenShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        const auto shadowCode = compile(ShadowSource, "light-shadow", "ps_5_0");
        check(device->CreatePixelShader(shadowCode->GetBufferPointer(), shadowCode->GetBufferSize(), nullptr, shadowShader.ReleaseAndGetAddressOf()),
            "Creating the lights' shadow shader");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        check(device->CreateBlendState(&blend, alphaOnly.ReleaseAndGetAddressOf()), "Creating the lights' shadow blend");

        // The game's (one, one) for its lights, scaled by the shadow in the targets' alpha.
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_ALPHA;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&blend, shadowed.ReleaseAndGetAddressOf()), "Creating the lights' blend");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        rasterizer.ScissorEnable = TRUE;
        check(device->CreateRasterizerState(&rasterizer, scissored.ReleaseAndGetAddressOf()), "Creating the lights' rasterizer state");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        check(device->CreateDepthStencilState(&depth, noDepth.ReleaseAndGetAddressOf()), "Creating the lights' depth state");

        passBuffer = constantBuffer(sizeof(PassConstants), "Creating the lights' pass constants");
        sceneBuffer = constantBuffer(SceneConstants * 4, "Creating the lights' scene constants");
        lightBuffer = constantBuffer(LightConstants * 4, "Creating the lights' light constants");
        materialBuffer = constantBuffer(MaterialConstants * 4, "Creating the lights' material constants");

        // Last, so it marks a complete pipeline.
        const auto fullScreenCode = compile(FullScreenSource, "light-full-screen", "vs_5_0");
        check(device->CreateVertexShader(fullScreenCode->GetBufferPointer(), fullScreenCode->GetBufferSize(), nullptr,
                  fullScreenShader.ReleaseAndGetAddressOf()),
            "Creating the lights' vertex shader");
        log("Ray-traced lights ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("Ray-traced lights unavailable: ") + error.what());

        return false;
    }
}

// The game's light a shader draws, when its twin is there to draw it again.
const GameLight* gameLightOf(IUnknown* shader)
{
    if (!shader) return nullptr;

    std::shared_lock lock(shaderMutex);
    const auto found = gameShaders.find(shader);
    if (found == gameShaders.end() || !twins[static_cast<std::size_t>(found->second->twin)]) return nullptr;

    return found->second;
}

void write(ID3D11Buffer* buffer, const void* data, std::size_t size)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(immediate->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Writing the lights' constants");
    std::memcpy(mapped.pData, data, size);
    immediate->Unmap(buffer, 0);
}

// What the light's lighting draw binds, or nothing when its constants are unknown.
std::optional<Light> noteOf(ID3D11DeviceContext* context, const GameLight* game)
{
    Light noted{.game = game};
    std::array<ID3D11Buffer*, 3> constants{};
    context->PSGetConstantBuffers(FirstConstants, static_cast<UINT>(constants.size()), constants.data());
    const bool known = gi_inputs::written(constants[0], noted.scene) && gi_inputs::written(constants[1], noted.light)
        && gi_inputs::written(constants[2], noted.material);
    for (auto* buffer : constants)
        if (buffer) buffer->Release();
    if (!known) return std::nullopt;

    std::array<ID3D11ShaderResourceView*, ResourceSlots> resources{};
    context->PSGetShaderResources(0, ResourceSlots, resources.data());
    for (UINT i = 0; i < ResourceSlots; ++i) noted.resources[i].Attach(resources[i]);

    std::array<ID3D11SamplerState*, SamplerCount> samplers{};
    context->PSGetSamplers(FirstSampler, SamplerCount, samplers.data());
    for (UINT i = 0; i < SamplerCount; ++i) noted.samplers[i].Attach(samplers[i]);

    std::array<ID3D11RenderTargetView*, 2> targets{};
    context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), nullptr);
    for (std::size_t i = 0; i < targets.size(); ++i) noted.targets[i].Attach(targets[i]);

    UINT viewports = 1;
    context->RSGetViewports(&viewports, &noted.viewport);
    if (!viewports || !noted.targets[0] || !noted.targets[1]) return std::nullopt;

    // A brighter flashlight: its colour times the brightness, and its falloff's d^16 factor over the
    // brightness^8, which puts the end of its reach (factor^(-1/16)) the brightness's square root further.
    if (game->flashlight)
    {
        const float brightness = flashlightBrightness.load(std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) noted.light[LightColor + i] *= brightness;
        noted.light[LightFalloff + 2] /= std::pow(brightness, 8.0f);
    }

    return noted;
}

// How far the light reaches, from its falloff; a point light's distance has c6.x added first.
float rangeOf(const Light& light)
{
    const float* falloff = &light.light[LightFalloff];
    const float factor = light.game->falloff == Falloff::Quartic ? falloff[3] : falloff[2];
    if (!(factor > 0)) return UnendingRange;

    const float reach = std::pow(factor, light.game->falloff == Falloff::Quartic ? -1.0f / 4 : -1.0f / 16);

    return std::max(light.game->shape == Shape::Point ? reach - falloff[0] : reach, 0.0f);
}

// The light passes' view space to the world, through cPSScene c8 to c10; `w` 1 for a point, 0 for a way.
std::array<float, 3> toWorld(const Light& light, const float* view, float w)
{
    std::array<float, 3> world{};
    for (int row = 0; row < 3; ++row)
    {
        const float* m = &light.scene[ViewToWorld + 4 * row];
        world[row] = m[0] * view[0] + m[1] * view[1] + m[2] * view[2] + m[3] * w;
    }

    return world;
}

std::array<float, 3> normalized(std::array<float, 3> v)
{
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0)
        for (auto& value : v) value /= length;

    return v;
}

gi_renderer::LocalLight forRays(const Light& light)
{
    const bool spot = light.game->shape == Shape::Spot;
    const bool sixteenth = light.game->falloff == Falloff::Sixteenth;
    const float* falloff = &light.light[LightFalloff];

    return {.position = toWorld(light, &light.light[LightPosition], 1),
        .axis = spot ? normalized(toWorld(light, &light.light[LightAxis], 0)) : std::array<float, 3>{0, 1, 0},
        .range = rangeOf(light),
        .cosOuter = spot ? light.light[LightUmbra] : PointCone,
        .clearance = std::max(LightClearance, light.light[LightSize]),
        .penumbraScale = spot ? light.light[LightCone + 1] : 0,
        .coneExponent = spot ? light.light[LightCone + 2] : 0,
        .maxFalloff = spot ? light.material[MaxFalloff] : std::numeric_limits<float>::max(),
        .color = {light.light[LightColor], light.light[LightColor + 1], light.light[LightColor + 2]},
        .sixteenth = sixteenth,
        .factor = sixteenth ? falloff[2] : falloff[3],
        .addedDistance = spot ? 0 : falloff[0],
        .leastDistance = spot ? 0 : light.material[LeastDistance],
        .radius = light.light[LightSize] > 0 ? light.light[LightSize] : BulbRadius,
        .flashlight = light.game->flashlight};
}

// Drawn for the frame `camera` draws: the same eye, within a millimetre. Lists recorded before the kick
// are this frame's, unless one was left from an earlier frame or the next frame's recording started early.
bool sameFrame(const Light& light, const camera::SceneConstants& camera)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(light.scene[Eye + i] - camera.c[20][i]) > 1e-3f) return false;

    return true;
}

// The pixels of the viewport the light can reach: its sphere, or for a spot the bounding sphere of its
// cone, on screen, as shadows.cpp's boundsOf puts a lamp's sphere there (the planes through the eye
// that touch it). The whole viewport when the camera plane cuts the sphere; none when it is behind or off
// screen.
std::optional<D3D11_RECT> reachOf(const Light& light)
{
    const float range = rangeOf(light);
    const float* position = &light.light[LightPosition];
    std::array<float, 3> centre{position[0], position[1], position[2]};
    float radius = range;

    // Up to a 60 degree cone, the sphere through its apex and the rim of its end holds it all.
    const float cosine = light.light[LightUmbra];
    if (light.game->shape == Shape::Spot && cosine >= 0.5f)
    {
        const auto axis = normalized({light.light[LightAxis], light.light[LightAxis + 1], light.light[LightAxis + 2]});
        radius = range / (2 * cosine);
        for (int i = 0; i < 3; ++i) centre[i] += axis[i] * radius;
    }

    const auto& viewport = light.viewport;
    const LONG left = static_cast<LONG>(viewport.TopLeftX), top = static_cast<LONG>(viewport.TopLeftY);
    const std::array<float, 2> size{viewport.Width, viewport.Height};
    const D3D11_RECT whole{left, top, left + static_cast<LONG>(size[0]), top + static_cast<LONG>(size[1])};
    const float z = centre[2];
    if (z + radius <= 0) return std::nullopt;
    if (z - radius <= 0) return whole;

    const float* projection = &light.scene[ProjectionParam];
    const float* offset = &light.scene[CenterOffset];
    const float clearance = z * z - radius * radius;
    std::array<LONG, 2> low{}, high{};
    for (int axis = 0; axis < 2; ++axis)
    {
        const float across = centre[axis];
        const float spread = radius * std::sqrt(across * across + clearance);
        const float scale = axis == 0 ? 0.5f : -0.5f;
        const auto pixelAt = [&](float t) { return ((t / projection[axis] + offset[axis]) * scale + 0.5f) * size[axis]; };
        const float first = pixelAt((across * z - spread) / clearance), second = pixelAt((across * z + spread) / clearance);
        low[axis] = static_cast<LONG>(std::clamp(std::floor(std::min(first, second)) - 1, 0.0f, size[axis]));
        high[axis] = static_cast<LONG>(std::clamp(std::ceil(std::max(first, second)) + 1, 0.0f, size[axis]));
        if (low[axis] >= high[axis]) return std::nullopt;
    }

    return D3D11_RECT{left + low[0], top + low[1], left + high[0], top + high[1]};
}

std::pair<UINT, UINT> sizeOf(ID3D11View* view)
{
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    view->GetResource(resource.GetAddressOf());
    if (resource && SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);

    return {desc.Width, desc.Height};
}

// The light once more: its shadow into the targets' alpha, then the game's light of its kind with
// specular scaled by it. Call with resourceMutex held, the pipeline made.
void drawAgain(const Light& light)
{
    const auto reach = reachOf(light);
    if (!reach) return;

    const auto [width, height] = sizeOf(light.targets[0].Get());
    if (!width || !height) return;

    ID3D11ShaderResourceView* share = light.traced ? global_illumination::localLightShadow(width, height) : nullptr;
    ++(share ? drawnShadowed : drawnUnshadowed);

    const auto& viewport = light.viewport;
    const PassConstants pass{{viewport.Width / static_cast<float>(width), viewport.Height / static_cast<float>(height),
                                 viewport.TopLeftX / static_cast<float>(width), viewport.TopLeftY / static_cast<float>(height)},
        {0, share ? 1u : 0u, 0, 0}};
    auto constants = light.light;
    if (!light.game->specular) constants[SpecularScale] = AddedSpecular;
    write(passBuffer.Get(), &pass, sizeof(pass));
    write(sceneBuffer.Get(), light.scene.data(), sizeof(light.scene));
    write(lightBuffer.Get(), constants.data(), sizeof(constants));
    write(materialBuffer.Get(), light.material.data(), sizeof(light.material));

    ID3D11RenderTargetView* targets[]{light.targets[0].Get(), light.targets[1].Get()};
    immediate->OMSetRenderTargets(2, targets, nullptr);
    immediate->RSSetViewports(1, &viewport);
    immediate->RSSetScissorRects(1, &*reach);

    immediate->PSSetShader(shadowShader.Get(), nullptr, 0);
    immediate->PSSetConstantBuffers(0, 1, passBuffer.GetAddressOf());
    immediate->PSSetShaderResources(0, 1, &share);
    immediate->OMSetBlendState(alphaOnly.Get(), nullptr, 0xFFFFFFFF);
    immediate->Draw(3, 0);

    std::array<ID3D11ShaderResourceView*, ResourceSlots> resources{};
    for (UINT i = 0; i < ResourceSlots; ++i) resources[i] = light.resources[i].Get();
    std::array<ID3D11SamplerState*, SamplerCount> samplers{};
    for (UINT i = 0; i < SamplerCount; ++i) samplers[i] = light.samplers[i].Get();
    ID3D11Buffer* lightConstants[]{sceneBuffer.Get(), lightBuffer.Get(), materialBuffer.Get()};
    immediate->PSSetShader(twins[static_cast<std::size_t>(light.game->twin)].Get(), nullptr, 0);
    immediate->PSSetShaderResources(0, ResourceSlots, resources.data());
    immediate->PSSetSamplers(FirstSampler, SamplerCount, samplers.data());
    immediate->PSSetConstantBuffers(FirstConstants, 3, lightConstants);
    immediate->OMSetBlendState(shadowed.Get(), nullptr, 0xFFFFFFFF);
    immediate->Draw(3, 0);
}

// The frame's lights so far, on the immediate context; call with resourceMutex held. The command lists
// the immediate context runs next set their own state, so this only unbinds what it bound.
void drawFrameLights()
{
    if (frameLights.empty()) return;

    try
    {
        if (!ensurePipeline() || !immediate)
        {
            lost += frameLights.size();
            frameLights.clear();
            return;
        }

        const performance::Timed timed(immediate, performance::Pass::LocalLights);
        immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        immediate->IASetInputLayout(nullptr);
        immediate->VSSetShader(fullScreenShader.Get(), nullptr, 0);
        immediate->VSSetConstantBuffers(0, 1, passBuffer.GetAddressOf());
        immediate->GSSetShader(nullptr, nullptr, 0);
        immediate->RSSetState(scissored.Get());
        immediate->OMSetDepthStencilState(noDepth.Get(), 0);
        for (const auto& light : frameLights) drawAgain(light);
    }
    catch (const std::exception& error)
    {
        log(std::string("Ray-traced lights: ") + error.what());
    }

    frameLights.clear();
    std::array<ID3D11ShaderResourceView*, ResourceSlots> noResources{};
    std::array<ID3D11Buffer*, 5> noConstants{};
    immediate->PSSetShaderResources(0, ResourceSlots, noResources.data());
    immediate->PSSetConstantBuffers(0, static_cast<UINT>(noConstants.size()), noConstants.data());
    immediate->VSSetConstantBuffers(0, 1, noConstants.data());
    immediate->OMSetRenderTargets(0, nullptr, nullptr);
    immediate->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    immediate->OMSetDepthStencilState(nullptr, 0);
    immediate->RSSetState(nullptr);
    immediate->VSSetShader(nullptr, nullptr, 0);
    immediate->PSSetShader(nullptr, nullptr, 0);
}

// Diagnostics: what became of the lights, every ten seconds while any came.
void logLights()
{
    static std::uint64_t lastLog{};
    const auto now = GetTickCount64();
    if (now - lastLog < 10'000) return;

    lastLog = now;
    const auto shadowedCount = drawnShadowed.exchange(0), unshadowedCount = drawnUnshadowed.exchange(0);
    const auto otherCount = otherFrame.exchange(0), overCount = overLimit.exchange(0), lostCount = lost.exchange(0);
    if (shadowedCount + unshadowedCount + otherCount + overCount + lostCount == 0) return;

    log("Lights in ten seconds: " + std::to_string(shadowedCount) + " drawn with their ray-traced shadow, " + std::to_string(unshadowedCount)
        + " without (recorded after the kick, or not traced); at the kicks " + std::to_string(otherCount) + " left for another frame's camera, "
        + std::to_string(overCount) + " over the " + std::to_string(GI_MAX_LOCAL_LIGHTS) + " the rays take; " + std::to_string(lostCount)
        + " lost (no draw point after them).");
}
}

void attach(ID3D11Device* gameDevice, ID3D11DeviceContext* gameImmediate) noexcept
{
    bool enable{};
    {
        std::lock_guard lock(optionsMutex);
        enable = wanted.enabled;
    }

    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    immediate = gameImmediate;
    if (enable) ensurePipeline();
}

void detach() noexcept
{
    enabled = active = false;
    {
        std::lock_guard lock(resourceMutex);
        fullScreenShader.Reset();
        shadowShader.Reset();
        alphaOnly.Reset();
        shadowed.Reset();
        scissored.Reset();
        noDepth.Reset();
        passBuffer.Reset();
        sceneBuffer.Reset();
        lightBuffer.Reset();
        materialBuffer.Reset();
        frameLights.clear();
        drawPointRan = false;
        device.Reset();
        immediate = nullptr;
        pipelineFailed = false;
    }

    {
        std::lock_guard lock(notesMutex);
        byContext.clear();
        byList.clear();
    }

    std::unique_lock lock(shaderMutex);
    gameShaders.clear();
    shaderOwners.clear();
    for (auto& twin : twins) twin.Reset();
}

void configure(const Options& options) noexcept
{
    {
        std::lock_guard lock(optionsMutex);
        wanted = options;
    }

    flashlightBrightness = options.flashlightBrightness;

    if (!options.enabled) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

std::string problem()
{
    {
        std::lock_guard lock(optionsMutex);
        if (!wanted.enabled) return {};
    }

    {
        std::lock_guard lock(resourceMutex);
        if (pipelineFailed) return "Ray-traced lights are unavailable (see PhantomFX.log).";
    }

    {
        std::shared_lock lock(shaderMutex);
        if (std::none_of(twins.begin(), twins.end(), [](const auto& twin) { return twin != nullptr; }))
            return "The game has not loaded its light shaders yet.";
    }

    return global_illumination::problem();
}

void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    const auto text = toHex(*checksum);
    const auto game = std::find_if(std::begin(GameLights), std::end(GameLights), [&](const auto& known) { return text == known.checksum; });
    if (game == std::end(GameLights)) return;

    try
    {
        std::unique_lock lock(shaderMutex);
        gameShaders[shader] = &*game;
        shaderOwners.emplace_back(shader);
        for (std::size_t i = 0; i < std::size(TwinChecksums); ++i)
            if (text == TwinChecksums[i] && !twins[i]) check(shader->QueryInterface(IID_PPV_ARGS(&twins[i])), "Taking one of the game's light shaders");
    }
    catch (...)
    {
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!active.load(std::memory_order_relaxed) || !call.indexed || call.instances != 1) return false;

    try
    {
        ComPtr<ID3D11PixelShader> gameShader;
        context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
        const auto* game = gameLightOf(gameShader.Get());
        if (!game) return false;

        // The stencil-marking draw writes no colour: drawn as the game does, but here, so that lamp
        // contact shadows (shadows.hpp) do not trace a light the rays shadow.
        ComPtr<ID3D11BlendState> blend;
        float blendFactor[4]{};
        UINT sampleMask{};
        context->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
        D3D11_BLEND_DESC desc{};
        if (blend) blend->GetDesc(&desc);
        if (blend && desc.RenderTarget[0].RenderTargetWriteMask == 0)
        {
            context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
            return true;
        }

        auto noted = noteOf(context, game);
        if (!noted) return false;

        // The lighting draw keeps its stencil work, without its light: no pixel shader.
        context->PSSetShader(nullptr, nullptr, 0);
        context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
        context->PSSetShader(gameShader.Get(), nullptr, 0);

        std::lock_guard lock(notesMutex);
        byContext[reinterpret_cast<std::uint64_t>(context)].lights.push_back(std::move(*noted));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void noteDrawPoint(ID3D11DeviceContext* context) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    try
    {
        std::lock_guard lock(notesMutex);
        byContext[reinterpret_cast<std::uint64_t>(context)].drawPoint = true;
    }
    catch (...)
    {
    }
}

void recorded(std::uint64_t list, std::uint64_t context) noexcept
{
    try
    {
        std::lock_guard lock(notesMutex);
        const auto found = byContext.find(context);
        if (found == byContext.end()) return;

        byList[list] = std::move(found->second);
        byContext.erase(found);
    }
    catch (...)
    {
    }
}

void forget(std::uint64_t list) noexcept
{
    std::lock_guard lock(notesMutex);
    byList.erase(list);
}

std::vector<gi_renderer::LocalLight> takeForTrace(const camera::SceneConstants& camera) noexcept
{
    std::vector<gi_renderer::LocalLight> lights;
    if (!active.load(std::memory_order_relaxed)) return lights;

    try
    {
        std::lock_guard lock(notesMutex);

        // The frame's lights by the pixels they can reach, most first, then the nearest: past what the rays can
        // take the smallest go without, the same ones frame after frame. Lights that reach none are never drawn again.
        struct Reaching
        {
            LONG pixels;
            float distance;
            Light* light;
        };
        std::vector<Reaching> reaching;
        for (auto& [list, noted] : byList)
            for (auto& light : noted.lights)
            {
                if (light.traced) continue;
                if (!sameFrame(light, camera))
                {
                    ++otherFrame;
                    continue;
                }

                const auto reach = reachOf(light);
                if (!reach) continue;

                const float* position = &light.light[LightPosition];
                reaching.push_back({(reach->right - reach->left) * (reach->bottom - reach->top),
                    std::sqrt(position[0] * position[0] + position[1] * position[1] + position[2] * position[2]), &light});
            }

        std::sort(reaching.begin(), reaching.end(),
            [](const Reaching& a, const Reaching& b) { return a.pixels != b.pixels ? a.pixels > b.pixels : a.distance < b.distance; });
        if (reaching.size() > GI_MAX_LOCAL_LIGHTS)
        {
            overLimit += reaching.size() - GI_MAX_LOCAL_LIGHTS;
            reaching.resize(GI_MAX_LOCAL_LIGHTS);
        }

        for (const auto& each : reaching)
        {
            each.light->traced = true;
            lights.push_back(forRays(*each.light));
        }
    }
    catch (...)
    {
        lights.clear();
    }

    return lights;
}

void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    Notes noted;
    {
        std::lock_guard lock(notesMutex);
        const auto found = byList.find(list);
        if (found == byList.end()) return;

        noted = std::move(found->second);
        byList.erase(found);
    }

    std::lock_guard lock(resourceMutex);
    if (context != immediate) return;

    try
    {
        for (auto& light : noted.lights) frameLights.push_back(std::move(light));
    }
    catch (...)
    {
    }

    // The first list with a draw point comes after the game's lights, and waits for the rays: the lights so
    // far are drawn before it. Any in a list after it are drawn at once.
    drawPointRan |= noted.drawPoint;
    if (drawPointRan) drawFrameLights();
}

void present() noexcept
{
    bool sawDrawPoint{};
    {
        std::lock_guard lock(resourceMutex);
        lost += frameLights.size();
        frameLights.clear();
        sawDrawPoint = drawPointRan;
        drawPointRan = false;
    }

    {
        // Notes of lists that never ran go, as do a context's left unfinished.
        std::lock_guard lock(notesMutex);
        for (auto* notes : {&byContext, &byList})
            std::erase_if(*notes, [](auto& entry) { return ++entry.second.presents > 3; });
    }

    bool on{};
    {
        std::lock_guard lock(optionsMutex);
        current = wanted;
        on = current.enabled;
    }

    {
        std::lock_guard lock(resourceMutex);
        on = on && fullScreenShader && !pipelineFailed;
    }

    {
        std::shared_lock lock(shaderMutex);
        on = on && std::any_of(twins.begin(), twins.end(), [](const auto& twin) { return twin != nullptr; });
    }

    enabled = on;
    active = on && sawDrawPoint;
    if constexpr (DiagnosticLog) logLights();
}
}
