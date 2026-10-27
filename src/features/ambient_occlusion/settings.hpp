#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct AmbientOcclusion
{
    bool on{};
    float radiusMetres{1};
    float strength{1};

    bool operator==(const AmbientOcclusion&) const = default;
};

void describe(AmbientOcclusion& occlusion, SettingVisitor& visit);
}
