#pragma once

#include "features/setting_visitor.hpp"

#include <string_view>

namespace phantom_fx::upscaler
{
// Frames presented per rendered frame, by DLSS Frame Generation up to Dynamic, which chooses them to
// reach the display's refresh rate. Fsr is FSR frame generation, 2x on any GPU.
enum class FrameGenerationMode : std::uint8_t { Off, X2, X3, X4, X5, X6, Dynamic, Fsr, Count };

std::string_view label(FrameGenerationMode mode);
// 2 to 6 for the fixed DLSS modes, 0 for Off, Dynamic and FSR.
unsigned multiplier(FrameGenerationMode mode);
}

namespace phantom_fx
{
// Turning it on, or switching between DLSS and FSR, takes a restart: the game then presents through
// Direct3D 12 (presenter.hpp).
struct FrameGeneration
{
    upscaler::FrameGenerationMode mode{upscaler::FrameGenerationMode::Off};

    bool operator==(const FrameGeneration&) const = default;
};

void describe(FrameGeneration& generation, SettingVisitor& visit);
}
