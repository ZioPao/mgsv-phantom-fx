#pragma once

#include <array>
#include <filesystem>

struct ID3D11Device;
struct ID3D11Texture2D;

// NVIDIA DLSS Ray Reconstruction in place of DLSS (upscaler.cpp): one model that denoises the ray-traced
// light and upscales. NGX's Direct3D 11 path for it crashes on its first evaluation, inside NVIDIA's
// code (310.9.1 and 310.9.16: NGXCubinD3D11::MarkBufferResourceAsAccessWrittenD3D11 reads a dispatch's
// tracking that nothing has set yet, 2026-10-30). So, as FSR (fsr_upscaler.hpp), it runs on a native
// Direct3D 12 device on the game's GPU, on textures Direct3D 11 shares, the two handing over through a
// shared fence.
namespace phantom_fx::ray_reconstruction
{
// One frame's inputs: Direct3D 11 textures created shared. All but the output are at render size; the
// feature is set up for the sizes of the colour and output textures.
struct Inputs
{
    ID3D11Texture2D* color{};  // Linear light, the game's white at 1
    ID3D11Texture2D* depth{};  // Device depth, reversed-Z
    ID3D11Texture2D* motion{}; // In render pixels toward the previous frame, without the jitter
    ID3D11Texture2D* output{};
    // The guides (ray_tracing::ReconstructionGuides): the world-space normal with the roughness in w,
    // the diffuse and the specular albedo.
    ID3D11Texture2D* normalRoughness{};
    ID3D11Texture2D* diffuseAlbedo{};
    ID3D11Texture2D* specularAlbedo{};
    std::array<float, 16> worldToView{}, viewToClip{}; // For row vectors (camera::StreamlineCamera)
    float jitter[2]{};                                 // In render pixels, +Y down
    float frameTime{};                                 // Milliseconds since the previous frame
    int quality{};                                     // NVSDK_NGX_PerfQuality_Value
    bool hdr{}, reset{};
};

// On the thread executing Direct3D work, as everything below. Starts NGX on a Direct3D 12 device on
// `device`'s GPU, with the model DLLs in `directory`. Throws when Ray Reconstruction cannot run here.
void start(ID3D11Device* device, const std::filesystem::path& directory);
bool started() noexcept;
// Upscales at this point of the device's immediate context: Direct3D 12 waits for the work recorded so
// far, and the work recorded after waits for the output. Returns true when a new feature, with a new
// history, was made. Throws on failure.
bool evaluate(const Inputs& inputs);
// Frees the feature, for DLSS; NGX and Direct3D 12 stay for a quick switch back.
void release() noexcept;
// Frees the feature, NGX and Direct3D 12, at the device's end. Not sooner: shutting NGX down on Direct3D 12
// broke frame generation's feature on the presenter's device (Streamline: "NGX evaluate feature failed
// 0xbad00004" from that moment, 2026-10-30).
void stop() noexcept;
}
