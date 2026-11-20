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
- `DevKit/commands/` — `<name>.cmd` files, one line at a time: `lua <code>`, `state`, `travel
  Afghanistan|Africa|MotherBase`, `cutscene <index>`, and the runner's `wait <frames>`.
- `DevKit/saves/`, `DevKit/local/` — when `saves/` exists, the game's Steam Cloud and user data
  folder are redirected there, as PhantomFX's automation copy does. Have only one of the two
  redirects in place: each installs its own Steam hooks.

The page is ReShade's overlay entry **DevKit**.

## With PhantomFX

When `PhantomFX.addon64` is loaded, DevKit registers its frame work on PhantomFX's main-loop hook
through the `PhantomFXRegisterFrameStart` export, so both add-ons run together with one hook. Without
PhantomFX, DevKit hooks the loop itself. PhantomFX keeps its own probes, cases and remaster panel
(`src/dev/`); DevKit has none of them.
