#pragma once

#include "game_constants.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <vector>

// What the ray tracing takes from the game's draws (docs/DEVELOPMENT.md, "Global illumination"): the
// constants the game writes for them, the main camera the frame was drawn with, and its terrain's
// inputs (engine_scene::noteTerrain). The scene itself comes from the game's memory (engine_scene.hpp).
// The screen-space shadows (contact_shadow_trace.hpp) read the light passes' constants from here too.
namespace phantom_fx::gi_inputs
{
// Releases what it holds; only when no Direct3D work can run.
void detach() noexcept;
// Inputs are noted only while on.
void setActive(bool active) noexcept;
// While on, the scene (b2) and light (b3) constants are kept even when not active, for the screen-space
// shadows.
void keepLightPassConstants(bool keep) noexcept;

// ---- Recording, on the game's render and worker threads ----
void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept;
// The constants the game writes with Map and Unmap right before the draws that read them: the
// scene (b2), material (b4), object (b5), light (b3) and probe (b7) constants.
void bufferMapped(ID3D11Resource* buffer, void* data) noexcept;
void bufferUnmapped(ID3D11Resource* buffer) noexcept;
// What this thread last wrote to `buffer`, `size` bytes of it into `into`; false when it wrote nothing,
// or, `exactly`, when that write had another size.
bool copyWritten(ID3D11Buffer* buffer, void* into, std::size_t size, bool exactly) noexcept;

// What this thread last wrote to `buffer` as C, whatever that write's size; none when it wrote nothing.
template<class C> std::optional<C> written(ID3D11Buffer* buffer) noexcept
{
    C read{};
    if (!copyWritten(buffer, &read, sizeof(C), false)) return std::nullopt;

    return read;
}

// The same, only when that write was exactly C's size.
template<class C> std::optional<C> writtenExactly(ID3D11Buffer* buffer) noexcept
{
    C read{};
    if (!copyWritten(buffer, &read, sizeof(C), true)) return std::nullopt;

    return read;
}

// What the pixel shader bound on `context` reads at C's slot, by writtenExactly.
template<class C> std::optional<C> pixelShaderReads(ID3D11DeviceContext* context) noexcept
{
    ID3D11Buffer* buffer{};
    context->PSGetConstantBuffers(C::Slot, 1, &buffer);
    auto read = writtenExactly<C>(buffer);
    if (buffer) buffer->Release();

    return read;
}

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
    game_constants::SceneConstants constants{};
    std::uint32_t gameFrame{};
};
std::optional<Camera> takeCamera() noexcept;

// A skinned mesh as the main camera's draw posed it: where its model is in the world, which of its
// meshes it is (the draw's index range), and the bones it drew with (game_constants::BonesConstants'
// bones).
struct DrawnBones
{
    std::array<float, 3> position{};
    std::array<float, 3> eye{}; // The draw's camera, for the frame it belongs to
    std::uint32_t firstIndex{}, indexCount{};
    std::array<float, 32 * 12> bones{};
};
// At the kick: the skinned meshes drawn for the frame `camera` draws since the last call.
std::vector<DrawnBones> takeDrawnBones(const game_constants::SceneConstants& camera) noexcept;

// Diagnostics: the hooks' time on the game's threads, all threads together, and the draws they
// looked at, since the last call.
struct Work
{
    double hookMilliseconds{};
    std::uint64_t draws{};
};
Work takeWork() noexcept;
}
