#pragma once

#include <d3d11_4.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The global illumination's Direct3D 12 side (docs/DEVELOPMENT.md, "Global illumination"): a device
// on the game's GPU that keeps the ray-traced scene (gi_scene.hpp) from the game's models, terrain and
// captured meshes, and traces it. Direct3D 11 hands it the frame's inputs in shared textures and the
// two take turns through a shared fence. Everything here runs on the game's render thread.
namespace phantom_fx::gi_renderer
{
// Textures Direct3D 11 made shareable (D3D11_RESOURCE_MISC_SHARED_NTHANDLE).
struct Textures
{
    ID3D11Texture2D* depth{};    // R32_FLOAT, reversed-Z
    ID3D11Texture2D* normal{};   // R10G10B10A2_UNORM, world space
    ID3D11Texture2D* albedo{};   // R8G8B8A8_UNORM, linear
    ID3D11Texture2D* transfer{}; // R32G32B32A32_FLOAT, captured vertices in world space
    ID3D11Texture2D* output{};   // R16G16B16A16_FLOAT, what Direct3D 11 reads back
    ID3D11Texture2D* shadow{};   // R16_FLOAT, the sun's ray-traced shadow; null while off
    ID3D11Texture2D* shadowDebug{}; // R8G8B8A8_UNORM, the shadow debug view (DebugShadows); null while off
};

using Matrix = std::array<float, 16>; // Rows applied as dot(row, v), as the engine's
using Transform = std::array<float, 12>; // Object to world: three rows applied as dot(row, (v, 1))
using Vertex = std::array<float, 3>;

// A mesh of a model: its index in the model file, whether its texture cuts holes in it (a cut-out
// mesh), its material's base texture (its colour, and the alpha a cut-out mesh cuts with: the path
// hash of its Base_Tex_SRGB, 0 without) or, for a mesh with none, its mask (Mask_Tex_LIN, the mask in
// red), its vertices in object space and, when it has them, their texture coordinates (two halves
// packed in a uint, a vertex each); its most detailed triangles, three indices each.
struct ModelPart
{
    std::uint32_t mesh{};
    bool alphaTested{};
    std::uint64_t baseTexture{};
    bool maskInRed{};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> uvs;
    std::vector<std::uint16_t> indices;
};

// A model the game has loaded (engine_scene.hpp): its meshes the scene takes.
struct Model
{
    std::uint64_t hash{}; // The model file's path hash, the same for every copy of the model
    std::vector<ModelPart> parts;
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

// One draw whose vertices the transfer texture holds this frame.
struct Capture
{
    std::uint32_t entry{}; // DynamicEntry for a skinned mesh
    std::uint32_t transferOffset{}; // First vertex
    std::uint32_t vertexCount{};
    // A skinned mesh: captured each frame it is drawn, and traced that frame only, in one
    // acceleration structure with the frame's other skinned meshes.
    bool dynamic{};
    bool alphaTested{}; // Its texture cuts holes in it, which its triangles lack
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
    std::array<float, 3> eye{}, previousEye{};
    float nearPlane{}, farPlane{};
    float verticalFov{}; // Radians
    float outputScale{}; // From the light as given (sun, sky) to the game's diffuse light units: the exposure
    // Towards the sun, world space; its diffuse light at normal incidence, before exposure and scaled
    std::array<float, 3> sunDirection{}, sunColor{};
    std::array<std::array<float, 3>, 9> skyRadiance{}; // L2 spherical harmonics, world space, in the sun colour's units
    std::vector<Capture> captures;
    std::vector<std::uint32_t> drawn; // Entries the game drew since the last frame
    std::vector<std::uint32_t> alphaTested; // Entries found to be alpha-tested since the last frame
    // The game's loaded models: those first seen since the last frame, and every placement this
    // frame (empty when no newer list came: the last one stays).
    std::vector<std::shared_ptr<const Model>> newModels;
    std::vector<ModelInstance> modelInstances;
    bool modelInstancesUpdated{};
    std::vector<SceneTexture> newTextures; // Copied, or copied again as the game streamed more of it in, since the last frame
    std::optional<Terrain> terrain; // Absent until the game's terrain draws give its inputs
    std::uint32_t frameIndex{};
    bool showScene{}; // The output shows the traced scene instead of the light
    bool traceLight = true; // A cost measurement leaves the light out: the scene is still built
    // The sun's ray-traced shadow into textures.shadow; the game's shadow map range (cPSLight c8.x,
    // c8.y, c6.x, and 1 once known) and the tangent of the sun's angular radius.
    bool traceShadows{};
    std::array<float, 4> shadowRange{};
    float sunTangent{};
    // Diagnostics, with the shadows: the debug view into textures.shadowDebug, and the probe at the
    // screen's centre (shadowProbe).
    bool debugShadows{};
    std::uint64_t waitValue{}, signalValue{}; // On the shared fence
};

struct Result
{
    // Captured meshes' entries whose geometry is gone, to capture again when the game draws them.
    std::vector<std::uint32_t> evicted;
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

// Captured meshes' entries, and how many vertices a frame's captures may hold.
constexpr std::uint32_t MaxEntries = 65536;
constexpr std::uint32_t TransferVertices = 4096 * 512;
// The entry the frame's skinned meshes share; captured static meshes take those before it.
constexpr std::uint32_t DynamicEntry = MaxEntries - 1;
// The alpha below which a cut-out mesh lets light through: the game's shadow pass discards below half its
// per-draw m_param.w (ModelForward_Shadow), measured 1 on cut-out meshes' draws (0 where it tests
// nothing); its G-buffer below m_param.w times a dither averaging a half.
constexpr float CutOutThreshold = 0.5f;

// For the overlay: entries, triangles and memory in use.
std::string status();
// For the overlay: the latest shadow probe as text, a few frames old; empty while the debug view is
// off. Logged by itself whenever what blocks the probe's sun ray changes.
std::string shadowProbe();
}
