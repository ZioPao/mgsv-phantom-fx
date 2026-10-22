#pragma once

#include "camera.hpp"
#include "gi_renderer.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The global illumination traces the game's own geometry (docs/DEVELOPMENT.md, "Global
// illumination", "Capture"). Direct3D 11 has no ray tracing and the game keeps no scene the add-on
// can read, so each mesh is captured as the game draws it: the draw is made once more through
// stream output, which records its triangles' clip positions. Static meshes are captured once and
// kept; skinned ones every frame. At the frame's kick the captures turn into world positions in
// the transfer texture, which Direct3D 12 builds the scene from (gi_renderer.hpp).
namespace phantom_fx::gi_capture
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run.
void detach() noexcept;
// Captures are made only while on.
void setActive(bool active) noexcept;
// A cost measurement pauses capturing: draws keep what the scene holds, and capture nothing.
void setCapturing(bool capturing) noexcept;

// ---- Recording, on the game's render and worker threads ----
void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept;
// The constants the game writes with Map and Unmap right before the draws that read them: the
// scene (b2), object (b5), light (b3) and probe (b7) constants.
void bufferMapped(ID3D11Resource* buffer, void* data) noexcept;
void bufferUnmapped(ID3D11Resource* buffer) noexcept;
// What this thread last wrote to `buffer`, as many floats as `into` holds; false when it wrote nothing.
bool written(ID3D11Buffer* buffer, std::span<float> into) noexcept;
// Before an indexed draw: captures it when it is a mesh the scene lacks. Never takes the draw.
void drawIndexed(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// FinishCommandList: what `context` recorded now belongs to `list`.
void recorded(std::uint64_t list, std::uint64_t context) noexcept;
void forget(std::uint64_t list) noexcept;

// ---- The immediate context ----
// Before the immediate context executes `list`.
void executing(std::uint64_t list) noexcept;

struct Converted
{
    std::vector<gi_renderer::Capture> captures;
    std::vector<std::uint32_t> drawn; // Entries the game drew since the last kick
    std::vector<std::uint32_t> alphaTested; // Entries found to be alpha-tested since the last kick
    // The main camera's scene constants (jittered) the last executed G-buffer draws had.
    std::optional<camera::SceneConstants> camera;
};

// At the kick: writes what the executed lists captured into `transfer` (GI_TRANSFER_WIDTH wide,
// gi_renderer::TransferVertices texels) on the immediate context. Throws on Direct3D errors.
Converted convert(ID3D11DeviceContext* immediate, ID3D11UnorderedAccessView* transfer);
// The renderer let these go; the next draw of each captures it again.
void evicted(const std::vector<std::uint32_t>& entries) noexcept;

// For the overlay.
std::string status();

// Diagnostics: the work on the game's threads since the last call.
struct Work
{
    double hookMilliseconds{}; // In the draw and constant buffer hooks, all threads together
    std::uint64_t draws{}, captures{}, skinned{}, vertices{};
};
Work takeWork() noexcept;
// Diagnostics: the scene's meshes held again under another key (the same place and size, other buffers
// or ranges), which of the draw's fields differ, and the new entries since the last call by source.
std::string keyChanges() noexcept;
}
