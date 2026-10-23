#pragma once

#include "gi_renderer.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The game's own scene for the ray tracing (docs/DEVELOPMENT.md, "Scene objects"): every rigid model
// the game has loaded, on screen or not, where it is and its triangles, read from the model
// visibility manager while the game's visibility tests run (nothing changes in it then), once a game
// frame. Draw capture (gi_capture.hpp) keeps what this leaves out: skinned and animated meshes,
// terrain, the instanced vegetation and rocks.
namespace phantom_fx::engine_scene
{
// Hooks the visibility tests on the supported build, with MinHook initialised; returns what was
// done, for the log. Without the hooks every mesh is captured as it is drawn.
std::string hook() noexcept;
// Snapshots are taken only while on.
void setActive(bool active) noexcept;

struct Snapshot
{
    bool instancesUpdated{}; // A newer list came: `instances` replaces the last, even when empty
    std::vector<gi_renderer::ModelInstance> instances;
    // Models first seen since the last take, read from their files.
    std::vector<std::shared_ptr<const gi_renderer::Model>> newModels;
};

// On the render thread: the latest snapshot's instances, when one came since the last call, and the
// new models, a few megabytes of model files a call at most.
void take(Snapshot& snapshot);

// The renderer let these models go: their files are read again when the game places them again.
void forget(const std::vector<std::uint64_t>& hashes);

// Whether a rigid draw with this world matrix (b5's m_world, c4 to c6 as twelve floats) is one of the
// last snapshot's instances: then the scene has it already.
bool covers(const float* worldRows) noexcept;

// Whether the scene takes the object brush's vegetation (its hooks are in and the scene is on); the
// clone arrays' placements are the visibility manager's instance groups. Their instanced draws (the
// _clone shaders) are not captured then.
bool clonesFromEngine() noexcept;

// The game's terrain as its terrain shaders draw it (NewTerrain_*; docs/DEVELOPMENT.md, "Scene
// objects"): the virtual heightmap's page table and height atlas (vertex shader t0, t1), the page
// table's converter (b4 c0, c1) and the world's scale and offset (b5 c10, c11).
struct TerrainInputs
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pages, heights;
    std::array<float, 8> converter{};   // c0, c1
    std::array<float, 8> scaleOffset{}; // c10, c11
};

// Before a terrain draw of the main view: notes its inputs, once a game frame, with the object and material
// constants the game wrote for it (the material's buffer holds other draws' by the time the frame is lit).
void noteTerrain(ID3D11DeviceContext* context, const float* objectConstants, const float* materialConstants) noexcept;

// On the render thread: the inputs last noted, once each.
std::optional<TerrainInputs> takeTerrain();

// Whether the scene makes the terrain from the game's heightmap; its draws are not captured then.
bool terrainFromEngine() noexcept;
void setTerrainFromEngine(bool on) noexcept;

// The game's loaded textures with these path hashes (docs/DEVELOPMENT.md, "Scene objects", the
// texture manager), each held so it outlives the game's own release; those not loaded are left out.
// On the render thread.
struct LoadedTexture
{
    std::uint64_t path{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
};
std::vector<LoadedTexture> loadedTextures(const std::vector<std::uint64_t>& paths);

// For the log.
std::string status();
}
