# Turn In Place

A turn-in-place mod for The Blood of Dawnwalker. When Coen stands still and the camera swings away from
where he faces, he turns on the spot with the game's own turn animations instead of only turning his head.

Status: in development. Turns in place while exploring (tested); combat, torch, crouch, horse, cutscenes and
dialogue are not tested yet.

## Requirements

- The Blood of Dawnwalker, Steam build 1.0.5 (25232147).
- UE4SS at commit `97b7e501`, from either:
  - Vercadi's UE4SS for Dawnwalker, rc6 (Nexus mod 18), or
  - Framecore's UE4SS for BoD, 2c or 2b, Performance or Compatibility profile (Nexus mod 283).

## Settings

`config/turninplace.ini`. Saved changes apply in game within a second, except `toggle_key`, which applies
at the next start. A missing file or key falls back to the default below, with one warning in the log.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `enabled` | `true` | `true`/`false`/`1`/`0` | Master switch |
| `turn_angle` | `50` | 46 to 170 | Degrees between camera and facing before a turn |
| `settle_speed` | `30` | 1 to 720 | Camera turning slower than this, in degrees per second, counts as settled |
| `settle_time` | `0.3` | 0 to 3 | Seconds settled before a turn starts |
| `chain_turns` | `true` | `true`/`false` | Keep turning while the camera keeps moving |
| `crouch` | `true` | `true`/`false` | Turn while crouched |
| `toggle_key` | blank | `F1`-`F12`, a letter or digit | Switches the mod on and off; blank is unbound |
| `log_level` | `normal` | `normal`/`verbose` | `verbose` logs every push and pop with its reason |

## Build

Needs Visual Studio 2022 (MSVC), CMake 3.22 or later, and an RE-UE4SS checkout at `97b7e501` with its
submodules and build prerequisites (see RE-UE4SS's own build instructions). The DLL links against
`UE4SS.dll`, so the checkout must match the loader's commit.

```
git -C <RE-UE4SS> checkout 97b7e501
cmake -S . -B <build dir> -G "Visual Studio 17 2022" -DDW_RE_UE4SS_SOURCE_DIR=<RE-UE4SS>
cmake --build <build dir> --config Game__Shipping__Win64 --target DWTurnInPlace --parallel
```

The first build compiles UE4SS too. The output is copied to `mod/dlls/main.dll` (and `main.pdb`). Use
`--parallel`, not `-- /m`: Git Bash rewrites `/m` into a path.

## Deploy

Close the game and copy `mod/` to `Dawnwalker/Binaries/Win64/ue4ss/Mods/DWTurnInPlace/`.

- `config/turninplace.ini` in an installed copy holds the player's settings: merge new keys into it, never
  overwrite it.
- `mod/dlls/main.pdb` never ships: it carries build paths.

## Layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Superbuild: RE-UE4SS from `DW_RE_UE4SS_SOURCE_DIR`, then the `DWTurnInPlace` target |
| `src/dllmain.cpp` | The DLL: the `AddMovementInput` vtable hook, player discovery, the push and pop state machine on the engine tick |
| `src/config.hpp` | Settings parsing for `turninplace.ini` |
| `mod/` | Exactly what ships under `ue4ss/Mods/DWTurnInPlace/`: `enabled.txt`, `LICENSE`, `config/turninplace.ini`; `dlls/` is build output |
| `docs/design.md` | How the game's turn in place works, the measurements behind the design, the plan |

## License

GPL-3.0-or-later. See `LICENSE`.
