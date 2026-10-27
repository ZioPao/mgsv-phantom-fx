#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct FrameRate
{
    bool unlocked{};

    bool operator==(const FrameRate&) const = default;
};

void describe(FrameRate& frameRate, SettingVisitor& visit);
}
