#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct RayTracedLights
{
    bool on{};
    int shadowRaysPerPixel{4};
    float flashlightBrightness{8};

    bool operator==(const RayTracedLights&) const = default;
};

void describe(RayTracedLights& lights, SettingVisitor& visit);
}
