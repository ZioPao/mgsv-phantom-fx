#pragma once

#include "camera.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <vector>

// What the ray tracing takes from the game's draws (docs/DEVELOPMENT.md, "Global illumination"): the
// constants the game writes for them, the main camera the frame was drawn with, and its terrain's
// inputs (engine_scene::noteTerrain). The scene itself comes from the game's memory (engine_scene.hpp).
namespace phantom_fx::gi_inputs
{
// Releases what it holds; only when no Direct3D work can run.
void detach() noexcept;
// Inputs are noted only while on.
void setActive(bool active) noexcept;

// ---- Recording, on the game's render and worker threads ----
void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept;
// The constants the game writes with Map and Unmap right before the draws that read them: the
// scene (b2), material (b4), object (b5), light (b3) and probe (b7) constants.
void bufferMapped(ID3D11Resource* buffer, void* data) noexcept;
void bufferUnmapped(ID3D11Resource* buffer) noexcept;
// What this thread last wrote to `buffer`, as many floats as `into` holds; false when it wrote nothing.
bool written(ID3D11Buffer* buffer, std::span<float> into) noexcept;
// Before an indexed draw of the game's models or terrain: notes the camera and the terrain's inputs.
void drawIndexed(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// FinishCommandList: what `context` noted now belongs to `list`.
void recorded(std::uint64_t list, std::uint64_t context) noexcept;
void forget(std::uint64_t list) noexcept;

// ---- The immediate context ----
// Before the immediate context executes `list`.
void executing(std::uint64_t list) noexcept;

// At the kick: the main camera's scene constants (jittered) of the draws executed since the last call,
// and the game loop's frame they were recorded in (game_loop.hpp); none when no such draw ran.
struct Camera
{
    camera::SceneConstants constants{};
    std::uint32_t gameFrame{};
};
std::optional<Camera> takeCamera() noexcept;

// A skinned mesh as the main camera's draw posed it: where its model is in the world, which of its
// meshes it is (the draw's index range), and the bones it drew with (VSBones' g_vsBone: 32 3x4 rows,
// the model's palette through the mesh's bone group).
struct DrawnBones
{
    std::array<float, 3> position{};
    std::array<float, 3> eye{}; // The draw's camera, for the frame it belongs to
    std::uint32_t firstIndex{}, indexCount{};
    std::array<float, 32 * 12> bones{};
};
// At the kick: the skinned meshes drawn for the frame `camera` draws since the last call.
std::vector<DrawnBones> takeDrawnBones(const camera::SceneConstants& camera) noexcept;

// Diagnostics: the hooks' time on the game's threads, all threads together, and the draws they
// looked at, since the last call.
struct Work
{
    double hookMilliseconds{};
    std::uint64_t draws{};
};
Work takeWork() noexcept;
}
