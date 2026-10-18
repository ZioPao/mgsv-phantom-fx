#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace mgsv_dlss::upscaler
{
// DLAA replaces the game's FXAA at native resolution; the other modes render below the output size
// and upscale. Off leaves the game as it is.
enum class Mode : std::uint8_t { Off, Dlaa, Quality, Balanced, Performance, UltraPerformance, Custom, Count };
// Values match NVSDK_NGX_DLSS_Hint_Render_Preset; Default lets the DLSS DLL choose.
enum class Preset : std::uint8_t { Default = 0, J = 10, K = 11, L = 12, M = 13 };

struct Settings
{
    static constexpr float MinScale = 0.33f, MaxScale = 1.0f, DefaultCustomScale = 0.75f;
    static constexpr float MinMipBias = -2.0f, MaxMipBias = 1.0f;
    static constexpr float DefaultSharpness = 0.33f, MaxSharpness = 1.0f;

    Mode mode{Mode::Dlaa};
    Preset preset{Preset::Default};
    // RCAS strength on the DLSS output; 0 leaves it unsharpened.
    float sharpness{DefaultSharpness};
    // Render width divided by output width in Custom mode.
    float customScale{DefaultCustomScale};
    // Added to the automatic log2(render / output) texture LOD bias.
    float mipBiasOffset{};

    // Lifts the game's 60 fps cap (frame_rate.hpp), whatever the mode.
    bool unlockFps{};

    bool operator==(const Settings&) const = default;
};

std::string_view label(Mode mode);
std::string_view label(Preset preset);
bool validPreset(int value);
// Quality, Balanced, Performance, Ultra Performance and Custom render below the output size.
bool upscales(Mode mode);

// Render width divided by output width. DLSS's own optimal sizes take precedence when NGX is ready.
float nominalScale(Mode mode, float customScale);
// Texture LOD bias that keeps output-resolution texture detail when rendering below it.
float mipBias(float scale, float offset);

// Missing files and invalid fields keep their defaults; I/O failures throw.
Settings loadSettings(const std::filesystem::path& path);
void saveSettings(const std::filesystem::path& path, const Settings& settings);
}
