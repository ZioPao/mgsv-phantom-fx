# Devkit

What a dev build of another MGSV add-on can build on: the tools that do not need PhantomFX to mean anything.
The add-on sets `devkit::setLogger` first, links `devkit` in its dev preset alone, and calls in from its own hooks.

| File | What it is |
| --- | --- |
| `log.*` | One logger function pointer, so the devkit writes to the add-on's log |
| `save_redirect.*` | An automated run's saves, Steam Cloud and user data folder kept off the player's |

`devkit.cmake` builds the static library. `src/dev/` is what stays PhantomFX's own: the Developer page, the debug
cases, and the probes into its passes.
