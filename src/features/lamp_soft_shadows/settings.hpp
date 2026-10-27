#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct LampSoftShadows
{
    bool on{};

    bool operator==(const LampSoftShadows&) const = default;
};

void describe(LampSoftShadows& shadows, SettingVisitor& visit);
}
