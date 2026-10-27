#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct Sun
{
    float diameterDegrees{0.53f};

    bool operator==(const Sun&) const = default;
};

void describe(Sun& sun, SettingVisitor& visit);
}
