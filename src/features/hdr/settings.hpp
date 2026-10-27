#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// HDR10 output (hdr.hpp). Turning it on or off takes a restart: the swap chain and the scene targets
// are made for it. The brightnesses apply at once.
struct Hdr
{
    bool on{};
    float peakNits{};
    float paperWhiteNits{203};

    bool operator==(const Hdr&) const = default;
};

void describe(Hdr& hdr, SettingVisitor& visit);
}
