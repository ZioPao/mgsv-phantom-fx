# DevKit add-on

The developer tools as their own ReShade add-on (`DevKit.addon64`), built in the dev preset beside
`PhantomFX.addon64` and not linked into it. It draws `src/devkit/page.*` in ReShade's default ImGui
style, and gets the game's frame start from PhantomFX when that is loaded (or hooks the loop itself)
for travel, cutscenes, the Lua console and command files.

| File | What it is |
| --- | --- |
| `addon.cpp` | ReShade's entry points: the log, the build check, the tools' install, the present event and the overlay page |
| `game_loop.*` | The main loop's frame start (0x7F840) and the devkit calls on it |

## Use

Copy `build/dev/DevKit.addon64` beside `mgsvtpp.exe` with ReShade 6.8. The add-on keeps its files in
`DevKit/` beside itself:

- `DevKit.log` — its log, including the reasons a tool did not run.
- `PhantomFX-automation/` — the automation (`src/devkit/automation.*`, its lines are listed in the header):
  `<name>.cmd` files, one line at a time, and F9's debug cases. The name is the one the
  `tools/automation` scripts know. When it holds `saves/` and `local/`, the game's Steam Cloud and user
  data folder are redirected there (the automation copy), or the game quits at once.

The page is ReShade's overlay entry **DevKit**.

## With PhantomFX

DevKit owns the automation; PhantomFX owns nothing of it but the parts that run inside its passes.
When `PhantomFX.addon64` is loaded, DevKit registers its frame work on PhantomFX's main-loop hook
through the `PhantomFXRegisterFrameStart` export, so both add-ons run together with one hook, and
reaches the held camera, the records, the costs, the probes, the scene and the settings through the
`PhantomFXGetDevApi` export (`src/devkit/phantom_api.hpp`, answered by `src/dev/dev_api.cpp`). Without
PhantomFX, DevKit hooks the loop itself, and the lines that stand on the game alone (`lua`, `travel`,
`cutscene`, `state`, `peek`) still run; the others say PhantomFX is not loaded. PhantomFX's own
Developer page keeps the remaster panel and the cost recorder (`src/dev/`).
