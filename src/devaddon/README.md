# DevKit add-on

The developer tools as their own ReShade add-on (`DevKit.addon64`), built in the dev preset beside
`PhantomFX.addon64` and not linked into it. It draws `src/devkit/page.*` in ReShade's default ImGui
style and hooks the game's main loop for travel, cutscenes, the Lua console and command files.

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
  folder are redirected there, as PhantomFX's automation copy does.

The page is ReShade's overlay entry **DevKit**.

## Limits

- It hooks the game's main loop. Do not run it with an add-on that already hooks that loop
  (PhantomFX does): the second hook sees the first's jump, reports unexpected code, and the frame
  tools stay off.
- No PhantomFX probes, cases or remaster panel: those are `src/dev/` and stay with PhantomFX.
