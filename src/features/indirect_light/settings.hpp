#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
struct IndirectLight
{
    bool on{};
    float radiusMetres{2};
    float strength{1};

    bool operator==(const IndirectLight&) const = default;
};

void describe(IndirectLight& light, SettingVisitor& visit);
}
