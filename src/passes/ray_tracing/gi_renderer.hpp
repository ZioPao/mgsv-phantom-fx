#pragma once

#include "passes/ray_tracing/scene_description.hpp"

#include <d3d11_4.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The global illumination's Direct3D 12 side (docs/DEVELOPMENT.md, "Global illumination"): a device
// on the game's GPU that keeps the ray-traced scene (gi_scene.hpp) from the game's models, characters
// and terrain, and traces it. Direct3D 11 hands it the frame's inputs in shared textures and the
// two take turns through a shared fence. Everything here runs on the game's render thread.
namespace phantom_fx::gi_renderer
{
using scene_description::CharacterInstance;
using scene_description::Model;
using scene_description::ModelInstance;
using scene_description::ModelPart;
using scene_description::Transform;

// Textures Direct3D 11 made shareable (D3D11_RESOURCE_MISC_SHARED_NTHANDLE).
struct Textures
{
    ID3D11Texture2D* depth{};    // R32_FLOAT, reversed-Z
    ID3D11Texture2D* normal{};   // R10G10B10A2_UNORM, world space
    ID3D11Texture2D* albedo{};   // R8G8B8A8_UNORM, linear
    ID3D11Texture2D* output{};   // R16G16B16A16_FLOAT, what Direct3D 11 reads back
    ID3D11Texture2D* shadow{};   // R16_FLOAT, the sun's ray-traced shadow; null while off
    ID3D11Texture2D* shadowDebug{}; // R8G8B8A8_UNORM, the shadow debug view (DebugShadows); null while off
    // A copy of the game's velocity for its moving meshes, for the denoisers (g_ObjectVelocity in
    // gi_common.hlsli), as the game's view reads it; null when this frame's did not come before the kick.
    ID3D11Texture2D* velocity{};
    DXGI_FORMAT velocityFormat{};
    ID3D11Texture2D* localLightShadow{}; // R16_FLOAT, the share of the point and spot lights' light that reaches; null without lights
    ID3D11Texture2D* lightDebug{}; // R8G8B8A8_UNORM, the light's denoiser history (CompositeLight); null while off
};

// A point or spot light for its shadow rays and the global illumination's hits (GILocalLight), world
// space: its light as the game's light shaders work it out, cut off past a range, and a spot's outside a
// cone.
struct LocalLight
{
    std::array<float, 3> position{}, axis{};
    float range{};
    float cosOuter{};  // The cosine of a spot's cone's outer edge; below -1 for a point
    float clearance{}; // How far short of the light its rays end: the lamp around it
    float penumbraScale{}, coneExponent{}; // A spot's cone: (cosine - cosOuter) x penumbraScale, saturated, to this power
    float maxFalloff{};                    // The most a spot's falloff gives
    std::array<float, 3> color{};          // Before exposure, as the game's cPSLight c4
    bool sixteenth{};                      // (1 - d^16 factor) / d^2 (Lighting_*_NA); else 1 / d^2 - d^2 factor (SSLighting2_*)
    float factor{};
    float addedDistance{}, leastDistance{}; // A point's: its distance is at least the least, plus the added
    float radius{};                         // Its shadow rays spread over a disc this wide around it
    float shadowStart{};                    // A shadow-mapped spot's: how far along its axis the game's shadow map starts; 0 for none
    // The bounds in the world of the game's volume for it (its box, a spot's cone within one), when known:
    // its light stops there, as the game's does.
    std::array<float, 3> volumeLowest{}, volumeHighest{};
    bool volumeKnown{};
};

using Matrix = std::array<float, 16>; // Rows applied as dot(row, v), as the engine's

// One of the game's textures for the scene, by its path hash (ModelPart::baseTexture): a shared copy
// of its levels of detail from the first no larger than the scene keeps (gi_textures.hpp), viewed as
// `viewFormat`.
struct SceneTexture
{
    std::uint64_t path{};
    ID3D11Texture2D* texture{};
    DXGI_FORMAT viewFormat{};
    std::uint32_t mips{};
};

// The game's terrain heightmap as its terrain shaders read it (engine_scene::TerrainInputs), shared
// copies of its most detailed level, for the rays' terrain rings (TerrainGrid in gi_scene.hlsl).
struct Terrain
{
    ID3D11Texture2D* pages{};   // The page table
    ID3D11Texture2D* heights{}; // The height atlas
    DXGI_FORMAT pagesFormat{}, heightsFormat{}; // As the game views them
    std::array<float, 4> scale{}, offset{}, uvConverter{}, texelCorrect{};
};

struct Frame
{
    Textures textures;
    std::uint32_t width{}, height{};
    Matrix viewProjection{}, previousViewProjection{};
    Matrix view{}, projection{}, previousView{}, previousProjection{}; // The engine's m_view and m_projection (cPSScene c8, c4)
    std::array<float, 4> jitter{}; // The projection's jitter in NDC (cPSScene c28): xy this frame's, zw the last frame's
    bool cameraCut{}; // The light's denoiser drops its history (camera::cameraCut)
    std::array<float, 3> eye{};
    float nearPlane{}, farPlane{};
    float verticalFov{}; // Radians
    float outputScale{}; // From the light as given (sun, sky) to the game's diffuse light units: the exposure
    // Towards the sun, world space; its diffuse light at normal incidence, before exposure and scaled
    std::array<float, 3> sunDirection{}, sunColor{};
    std::array<std::array<float, 3>, 9> skyRadiance{}; // L2 spherical harmonics, world space, in the sun colour's units
    // The game's loaded models: those first seen since the last frame, and every placement this
    // frame (empty when no newer list came: the last one stays).
    std::vector<std::shared_ptr<const Model>> newModels;
    std::vector<ModelInstance> modelInstances;
    std::vector<CharacterInstance> characters; // With modelInstances
    bool modelInstancesUpdated{};
    std::vector<SceneTexture> newTextures; // Copied, or copied again as the game streamed more of it in, since the last frame
    std::optional<Terrain> terrain; // Absent until the game's terrain draws give its inputs
    std::uint32_t frameIndex{};
    bool showScene{}; // The output shows the traced scene instead of the light
    bool logOften{};  // Diagnostics: the light's statistics logged every few frames (as each readback returns), not every ten seconds
    bool traceLight = true; // A cost measurement leaves the light out: the scene is still built
    // The sun's ray-traced shadow into textures.shadow; the game's shadow map range (cPSLight c8.x,
    // c8.y, c6.x, and 1 once known) and the tangent of the sun's angular radius.
    bool traceShadows{};
    std::array<float, 4> shadowRange{};
    float sunTangent{};
    // Diagnostics, with the shadows: the debug view into textures.shadowDebug, and the probe at the
    // screen's centre (shadowProbe).
    bool debugShadows{};
    bool shadowStarts{};     // The debug view: where the shadow rays leave each pixel (ShadowStartColor)
    bool twoSidedShadows{};  // Diagnostics: rays towards lights blocked by every surface from both sides, as until 2026-10-26
    std::uint32_t lightRays{2}; // The light's rays a light pixel, 1 to 4 (TraceLight)
    // The point and spot lights whose shadows go into textures.localLightShadow, at most GI_MAX_LOCAL_LIGHTS,
    // and the rays a pixel they take.
    std::vector<LocalLight> localLights;
    std::uint32_t localLightRays{1};
    bool probeLights{}; // Diagnostics: the lamp probe at the screen's centre (lightProbe), sun or none
    std::uint64_t waitValue{}, signalValue{}; // On the shared fence
};

struct Result
{
    // Models let go after a while unplaced, to read again when the game places them again, and the
    // textures no model left uses.
    std::vector<std::uint64_t> releasedModels, releasedTextures;
};

// Throws when the GPU cannot trace rays (DXR 1.1) or Direct3D 12 fails.
void start(ID3D11Device* device, HANDLE sharedFence);
void stop() noexcept;
bool started() noexcept;

// Records and submits the frame: waits for frame.waitValue on the shared fence, signals
// frame.signalValue when the output is written. Throws on Direct3D 12 errors.
Result render(const Frame& frame);

// For the overlay: instances, triangles and memory in use.
std::string status();
// For the overlay: the latest shadow probe as text, a few frames old; empty while the debug view is
// off. Logged by itself whenever what blocks the probe's sun ray changes.
std::string shadowProbe();
// The same of the lamp probe: the strongest point and spot lights at the screen's centre and what blocks
// each; logged whenever those change.
std::string lightProbe();
}
