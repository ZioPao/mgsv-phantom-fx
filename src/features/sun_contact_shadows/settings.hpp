#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct SunContactShadows
{
    bool on{};

    bool operator==(const SunContactShadows&) const = default;
};

void describe(SunContactShadows& shadows, SettingVisitor& visit);
}
