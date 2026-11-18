# Devkit

What a dev build of another MGSV add-on can build on: the tools that do not need PhantomFX to mean anything.
The add-on sets its name, its logger and its toasts first, links `devkit` in its dev preset alone, and calls in
from its own hooks.

| File | What it is |
| --- | --- |
| `host.*` | The add-on's name, in Lua chunk names, and where its toasts go |
| `log.*` | One logger function pointer, so the devkit writes to the add-on's log |
| `commands.*` | Commands from files: the runner, its waits, and the present thread's jobs |
| `save_redirect.*` | An automated run's saves, Steam Cloud and user data folder kept off the player's |
| `game.hpp` | The game's image and code checks, and the build check the add-on answers |
| `game_lua.*` | Lua in the game's own script state: expressions, print(), and the console's queue |
| `travel.*`, `cutscenes.*`, `cutscene_list.hpp` | Free roam in another map, and any cutscene the game's scripts name |

`devkit.cmake` builds the static library. The add-on answers `devkit::game::isSupportedBuild()` (PhantomFX:
`src/device_check.cpp`); everything else the devkit needs is in its folder. `src/dev/` is what stays PhantomFX's
own: the Developer page, the debug cases, and the probes into its passes.
