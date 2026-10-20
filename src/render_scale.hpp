#pragma once

#include <string>

// Has Fox Engine render its scene below the output size. docs/DEVELOPMENT.md, "Render size".
//
// The renderer keeps the size it renders at apart from the display's and rebuilds its targets when
// a wanted size differs from the current one. The add-on sets the wanted size; the swap chain, the
// window and the mouse stay at the display size.
namespace phantom_fx::render_scale
{
struct Size
{
    unsigned width{}, height{};

    bool operator==(const Size&) const = default;
};

// Hooks the renderer on the supported build, with MinHook initialised; returns what was done, for
// the log. Without the hook the engine renders at the display size.
std::string hook() noexcept;
bool hooked() noexcept;
// The size the engine renders at while it displays `display`; {0, 0} gives it back its own.
void request(Size render, Size display) noexcept;
// The size of the engine's scene targets now; {0, 0} before its first frame.
Size current() noexcept;
}
