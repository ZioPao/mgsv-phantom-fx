#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// The sea (sea.hpp): waves from an ocean spectrum, drawn into the G-buffer in place of the game's sea.
struct Sea
{
    bool on{};
    float waves{1};

    bool operator==(const Sea&) const = default;
};

void describe(Sea& sea, SettingVisitor& visit);
}
