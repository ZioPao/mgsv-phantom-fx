#pragma once

#include <filesystem>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;

// AMD FSR upscaling in place of DLSS (upscaler.cpp). AMD's runtime runs on Direct3D 12 only, so a
// native Direct3D 12 device on the game's GPU upscales textures Direct3D 11 shares, and the two hand
// over through a shared fence. The runtime runs FSR 4 on Radeon RX 7000 and 9000 and FSR 3.1 on
// other GPUs.
namespace phantom_fx::fsr_upscaler
{
// One frame's inputs: Direct3D 11 textures created shared. All but the output are at render size;
// FSR is set up for the sizes of the colour and output textures.
struct Inputs
{
    ID3D11Texture2D* color{};  // Linear light, the game's white at 1; above it with the scene in float
    ID3D11Texture2D* depth{};  // Device depth, reversed-Z
    ID3D11Texture2D* motion{}; // In render pixels toward the previous frame, without the jitter
    ID3D11Texture2D* output{};
    float jitter[2]{};               // In render pixels, +Y down
    float frameTime{};               // Milliseconds since the previous frame
    float cameraNear{}, cameraFar{}; // In metres
    float verticalFov{};             // In radians
    bool hdr{}, reset{};
};

// On the thread executing Direct3D work, as everything below. Loads AMD's runtime from `directory`
// and makes the Direct3D 12 device on `device`'s GPU. Throws when FSR cannot run here.
void start(ID3D11Device* device, const std::filesystem::path& directory);
// Upscales at this point of the device's immediate context: Direct3D 12 waits for the work recorded so
// far, and the work recorded after waits for the output. Returns true when FSR started a new history
// (a new context). Throws on failure.
bool evaluate(const Inputs& inputs);
// Frees FSR and Direct3D 12, for DLSS or the device's end.
void stop() noexcept;
// "FSR 4.1.1" or "FSR 3.1.5", once FSR has run; "FSR" before.
std::string name();
}
