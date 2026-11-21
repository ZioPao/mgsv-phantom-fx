#pragma once

#include <filesystem>

struct ID3D11DeviceContext;
struct ID3D11Resource;

// The automation: commands from files that let an agent look into and drive the running game, and F9's debug cases
// (docs/DEVELOPMENT.md, "Debug cases"). The command language, its runner, the cases and their waits are all here;
// what runs inside PhantomFX's passes (the held camera, the records, the costs, the probes, the scene and settings) is
// reached through PhantomFX's dev API (phantom_api.hpp), and the lines that need it say so when PhantomFX is not loaded.
//
// Files come through the `PhantomFX-automation` folder beside the add-ons (the name the tools/automation scripts know):
// each `<name>.cmd` runs, one line at a time, from the main loop's frame start, and `<name>.result` holds what each line
// did once all have run. With `saves` and `local` folders in it, the game is the automation copy: its saves are read
// from and written to there (save_redirect.hpp), or the game quits at once. Without them it is the player's game.
//
// Lines:
//   case [<name>]                 records a case: cases/<name>, read by tools/automation/case.py
//   freeze                        photo mode frozen at the camera now
//   release                       leaves the frozen camera, `probe`'s pixel and the debug view it turned on
//   camera <x> <y> <z> <yaw> <pitch> [<mm>]  photo mode frozen at that pose, radians, through a lens of that focal length
//   move <x> <y> <z> <yaw> <pitch> <frames> [<mm>]  from the frozen camera there, one step each camera update
//   wait <frames>                 lets that many frames present
//   wait_scene                    until the scene's work to do has stopped changing
//   probe <x> <y>                 the shadow and lamp probes' text at that render pixel
//   perf <file> [<frames>]        what the GPU's next frames cost, into `<file>.json`, 120 without saying
//   record_costs 0|1              the Developer page's Record the costs
//   inspect <file>                a record of every pixel, into `<file>.bin` and `<file>.tsv`
//   image <file>                  the next presented frame as `<file>.dds`
//   textures <folder>             the ray tracing's textures and the game's shadow, as DDS files
//   game_shadow 0|1               keeps the game's shadow per pixel
//   reveal off|all|<hash> ...     restarts the scene, taking what it leaves out: all of it or those models
//   enhancements 0|1              the F1 switch
//   setting <key>=<value>         a PhantomFX.ini line, applied at once (and saved, as the overlay does)
//   diagnostic <name> <value>     debug_view, show_scene, two_sided_shadows, no_jitter or river_light
//   lua <code>                    runs `code` in the game's Lua, its value or error
//   state [<name>]                what the game is at; with a name, into cases/<name>/state.txt
//   travel <map> [<x> <y> <z>]    afghanistan, africa or motherbase free roam, at that place
//   cutscene <index>              plays the cutscene at that index of the list
//   clock <seconds> [run]         the time of day, its clock stopped there, or running on from it
//   live 0|1                      a held camera leaves the game running, until released
//   weather <type> [<fog>]|off    forces TppDefine.WEATHER's type, with the fog's density for Foggy; or lets go
//   status                        frame counts, the camera, the scene
//   peek <address|game+rva> [bytes]  the game's memory there, 256 bytes without saying, 4096 at most
//   drawn <x> <y> <z> <radius>    where the main camera's rigid model draws of the next 10 frames put their models
//   owner <hash>                  every placement of that model the next snapshot finds
//   remaster <line>               the remastering tool's own actions
namespace devkit::automation
{
// Reads command files from `<addonDirectory>/PhantomFX-automation`, and redirects the saves there when it holds some.
void install(const std::filesystem::path& addonDirectory) noexcept;
// F9: from any thread; the game thread takes the case or lets the game go at its next frame start.
void caseKey() noexcept;
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;
// The render thread, as each frame presents its back buffer.
void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept;
}
