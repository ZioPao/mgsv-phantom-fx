#pragma once

#include "features/setting_visitor.hpp"

namespace phantom_fx
{
// Path tracing (path_tracing.hpp): with DLSS Ray Reconstruction denoising, the reflections, the sun and the
// point and spot lights traced too.
struct PathTracing
{
    bool on{};
    float reflections{1};
    float emission{1};
    // How much of the traced reflection a forward surface (water, glass) keeps. The water's own shader adds a
    // sky reflection that cannot be removed, so a full one would double; the glass's cube map is bound black.
    float forwardReflections{0.3f};

    bool operator==(const PathTracing&) const = default;
};

void describe(PathTracing& pathTracing, SettingVisitor& visit);
}
