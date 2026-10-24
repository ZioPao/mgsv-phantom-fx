#pragma once

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
};

using Matrix = std::array<float, 16>; // Rows applied as dot(row, v), as the engine's
using Transform = std::array<float, 12>; // Object to world: three rows applied as dot(row, (v, 1))
using Vertex = std::array<float, 3>;

// A mesh of a model: its index in the model file and its mesh group, whether its texture cuts holes
// in it (a cut-out mesh), its material's base texture (its colour, and the alpha a cut-out mesh cuts
// with: the path hash of its Base_Tex_SRGB, 0 without) or, for a mesh with none, its mask
// (Mask_Tex_LIN, cutting with its alpha), its vertices in object space and, when it has them, their texture
// coordinates (two halves packed in a uint, a vertex each); its most detailed triangles, three indices
// each. A skinned model's mesh also has each vertex's four bones (the model's bone numbers, 16 bits
// each, the first lowest) and their weights (8-bit unorms, the first lowest).
struct ModelPart
{
    std::uint32_t mesh{};
    std::uint32_t group{};
    bool alphaTested{};
    bool leaf{}; // Leaves, which let light through (GI_GEOMETRY_LEAF)
    std::uint64_t baseTexture{};
    bool textureIsMask{};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> uvs;
    std::vector<std::uint16_t> indices;
    std::vector<std::uint64_t> bones;
    std::vector<std::uint32_t> weights;
    // Where the game's draw of these triangles starts in the model's index buffer and how many it draws;
    // and a skinned mesh's palette slots, the model's bone in each (VSBones' g_vsBone, gi_inputs::DrawnBones).
    std::uint32_t firstIndex{}, indexCount{};
    std::vector<std::uint16_t> boneGroup;
    // Diagnostics (the shadow probe): the FMDL mesh's alpha and shadow bytes.
    std::uint8_t alphaBits{}, shadowBits{};
};

// Diagnostics (the shadow probe): a mesh of a model file the scene leaves out, with its alpha and shadow bytes.
struct LeftOutMesh
{
    std::uint32_t mesh{};
    std::uint8_t alphaBits{}, shadowBits{};
};

// A model the game has loaded (engine_scene.hpp): its meshes the scene takes; a skinned model's bones.
struct Model
{
    std::uint64_t hash{}; // The model file's path hash, the same for every copy of the model
    std::vector<ModelPart> parts;
    std::uint32_t bones{}; // 0: rigid, placed as a whole (ModelInstance); else posed (CharacterInstance)
    std::vector<LeftOutMesh> leftOut;
};

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

// One placement of a model, by its hash.
struct ModelInstance
{
    std::uint64_t hash{};
    Transform transform{};
};

// One of the game's characters (engine_scene.hpp): a skinned model in the pose the game gives it this
// frame. `id` stays the same while the game keeps it; `palette` has a 3x4 matrix (three rows of four)
// for each of the model's bones; `shownGroups` says which of its mesh groups the game draws.
struct CharacterInstance
{
    std::uint64_t id{};
    std::uint64_t hash{};
    Transform transform{};
    std::vector<float> palette;
    std::vector<bool> shownGroups;

    // Diagnostics (the shadow probe): each mesh group's state byte as read, and which of the model's
    // meshes (Model::parts) this frame's draws posed (engine_scene::take).
    std::vector<std::uint8_t> groupStates;
    std::vector<bool> drawnParts;
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
    bool cameraCut{}; // The light's denoiser drops its history (camera::cameraCut)
    std::array<float, 3> eye{}, previousEye{};
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
    bool shadowsFromDepth{}; // Diagnostics: every shadow ray from the depth and the G-buffer's normal, as before 2026-10-24
    bool opaqueLeaves{};     // Diagnostics: leaves let no light through (GI_GEOMETRY_LEAF), as before 2026-10-24
    // The point and spot lights whose shadows go into textures.localLightShadow, at most GI_MAX_LOCAL_LIGHTS,
    // and the rays a pixel they take.
    std::vector<LocalLight> localLights;
    std::uint32_t localLightRays{1};
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

// The alpha below which a cut-out mesh lets light through: the game's shadow pass discards below half its
// per-draw m_param.w (ModelForward_Shadow), measured 1 on cut-out meshes' draws (0 where it tests
// nothing); its G-buffer below m_param.w times a dither averaging a half.
constexpr float CutOutThreshold = 0.5f;

// For the overlay: instances, triangles and memory in use.
std::string status();
// For the overlay: the latest shadow probe as text, a few frames old; empty while the debug view is
// off. Logged by itself whenever what blocks the probe's sun ray changes.
std::string shadowProbe();
}
