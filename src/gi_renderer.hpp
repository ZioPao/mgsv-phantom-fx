#pragma once

#include <d3d11_4.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The global illumination's Direct3D 12 side (docs/DEVELOPMENT.md, "Global illumination"): a device
// on the game's GPU that keeps the captured geometry, builds the scene's acceleration structures
// and traces it. Direct3D 11 hands it the frame's inputs in shared textures and the two take turns
// through a shared fence. Everything here runs on the game's render thread.
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
    // Entries whose geometry is gone, to capture again when the game draws them.
    std::vector<std::uint32_t> evicted;
};

// Throws when the GPU cannot trace rays (DXR 1.1) or Direct3D 12 fails.
void start(ID3D11Device* device, HANDLE sharedFence);
void stop() noexcept;
bool started() noexcept;

// Records and submits the frame: waits for frame.waitValue on the shared fence, signals
// frame.signalValue when the output is written. Throws on Direct3D 12 errors.
Result render(const Frame& frame);

// The largest entry index the scene keeps, and how many triangles a capture may hold.
constexpr std::uint32_t MaxEntries = 65536;
constexpr std::uint32_t TransferVertices = 4096 * 512;
// The entry the frame's skinned meshes share; static meshes take the others.
constexpr std::uint32_t DynamicEntry = MaxEntries - 1;

// For the overlay: entries, triangles and memory in use.
std::string status();
// For the overlay: the latest shadow probe as text, a few frames old; empty while the debug view is
// off. Logged by itself whenever what blocks the probe's sun ray changes.
std::string shadowProbe();
}
