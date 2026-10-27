#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct MotionBlur
{
    bool on{true};
    float strength{1};

    bool operator==(const MotionBlur&) const = default;
};

void describe(MotionBlur& blur, SettingVisitor& visit);
}
