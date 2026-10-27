#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct LampContactShadows
{
    bool on{};

    bool operator==(const LampContactShadows&) const = default;
};

void describe(LampContactShadows& shadows, SettingVisitor& visit);
}
