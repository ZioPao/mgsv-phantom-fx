#pragma once

#include <filesystem>

// Development diagnostics, on only when a `PhantomFX-frame-log` folder exists beside the add-on as the
// game starts; otherwise no event is registered, so they cost nothing. They only read the game's
// Direct3D 11 state, so the frame renders unchanged. The files go into that folder:
// - Every shader the game creates is logged once to PhantomFX.log: stage, DXBC checksum, size. With a
//   `shaders` folder inside, the bytecode is saved there too.
// - F10 records a frame, present to present, to `frame-<n>.txt`: each draw, dispatch, clear and copy
//   with its shaders, targets, inputs and states, in the order the GPU runs them (command lists from
//   deferred contexts where they are executed). `frame-<n>-writers.txt` lists the call stacks that
//   wrote constant buffers.
// - `dump.txt`, lines of "<shader checksum> <count>", makes the first <count> calls with that shader
//   also record their constant buffers' contents and save their textures as DDS files in
//   `frame-<n>/`. A call inside a command list saves them once the list has run, so outputs show the
//   list's final state.
namespace phantom_fx::frame_log
{
// Registers the ReShade events when `addonDirectory` holds the PhantomFX-frame-log folder.
void install(const std::filesystem::path& addonDirectory);
bool installed();
void uninstall();
}
