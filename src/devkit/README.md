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

## Using it in another add-on

1. Bring `src/devkit/` into the add-on's tree (or make this repository a submodule and add its `src` to the
   include path), and build `devkit.cmake` as a static library in the dev preset alone. Link it only there. It
   needs MinHook, which `devkit.cmake` links for the saves redirection.
2. Answer the host side before installing anything:
   ```cpp
   devkit::setName("MyMod");   // Lua chunk names the game's errors show
   devkit::setLogger(&myLog);  // void(std::string_view)
   devkit::setToast([](devkit::Tone tone, std::string_view text, std::string_view detail) { myToast(tone, text, detail); });
   namespace devkit::game
   {
   bool isSupportedBuild() noexcept { return myBuildCheck(); }
   }
   ```
3. Hook it into the add-on's own frame:
   - `devkit::commands::install(folder, run, waiting, started)` once; `onFrameStart` from the game thread and
     `onPresent` from the render thread.
   - `run` gets each line and dispatches the add-on's commands; `wait <frames>` is the runner's own. `waiting`
     returns true while a line's work (a scene, a capture) is unfinished, and `started` resets that state for a
     new file.
   - Work for the render thread: `devkit::commands::askPresent([](context, backBuffer) { ...; return "saved\n"; })`;
     the runner waits and appends the result.
   - The add-on's own key can call `devkit::commands::start({"line", "line"})`.
4. Use the tools directly: `game_lua::run` and its console, `travel::go`, `cutscenes::play`,
   `save_redirect::install`, each with its `onFrameStart` called from the game thread. Paths for command results
   are `devkit::commands::folder()`.

The page is the add-on's: the devkit ships no UI and no `fxui`. PhantomFX's Developer page (`src/dev/dev.cpp`)
is the example; it maps `devkit::Tone` to `fxui::Tone` in one switch. What stays PhantomFX's: the Developer page,
the debug cases, and the probes into its passes (inspect, perf record, game shadow, held camera, remaster).

Release: link `devkit` only when the dev tools are on, and keep the dev-only strings listed in
`tools/automation/check_release.py` current; it reads the built add-on and fails if any reached it.
