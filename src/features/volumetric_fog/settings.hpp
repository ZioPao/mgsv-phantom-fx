#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// The game's own sky and fog colours, or Hillaire's physical sky and air.
enum class AtmosphereLook : std::uint8_t { Mgsv, Physical };

struct VolumetricFog
{
    bool on{};
    AtmosphereLook look{AtmosphereLook::Mgsv};
    float haze{};

    bool operator==(const VolumetricFog&) const = default;
};

void describe(VolumetricFog& fog, SettingVisitor& visit);
}
