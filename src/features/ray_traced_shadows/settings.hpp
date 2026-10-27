#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct RayTracedShadows
{
    bool on{};
    bool withGameShadows{};

    bool operator==(const RayTracedShadows&) const = default;
};

void describe(RayTracedShadows& shadows, SettingVisitor& visit);
}
