#pragma once

// PhantomFX's debug lines in the DevKit add-on's command files (devkit_link.cpp, ../devkit/extension.hpp).
namespace phantom_fx::devkit_link
{
// Plugs the lines in if DevKit has loaded; the frame start tries again until it has.
void install() noexcept;
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;
}
