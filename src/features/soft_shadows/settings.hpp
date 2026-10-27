#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct SoftShadows
{
    bool on{};

    bool operator==(const SoftShadows&) const = default;
};

void describe(SoftShadows& shadows, SettingVisitor& visit);
}
