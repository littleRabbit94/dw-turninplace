# Turn In Place

A turn-in-place mod for The Blood of Dawnwalker. When you stand still and the camera swings away from
where your character faces, your character turns on the spot with the game's own turn animations instead
of only turning the head.

Status: in development. Turns in place while standing and exploring (tested). Crouched, it stays out: the game
has no crouched turn for this. In combat the game's own combat turns run and the mod stays out. The torch is
not tested yet.

## Requirements

- The Blood of Dawnwalker, Steam build 1.0.5 (25232147).
- UE4SS at commit `97b7e501`, from either:
  - Vercadi's UE4SS for Dawnwalker, rc6 (Nexus mod 18), or
  - Framecore's UE4SS for BoD, 2c or 2b, Performance or Compatibility profile (Nexus mod 283).
- Optional: Dawnwalker Mod Menu (Nexus mod 271) for the in-game settings page. 1.0.5 or later hides the
  Turning settings while the mod is off; tested with 1.0.7. Without it, edit `config/turninplace.ini`.

## What it changes

- Only the character's rotation mode on the movement component: while a turn is needed it pushes
  `FaceDirection` (the game's own camera-facing rotation, also used in combat) at priority 1 and pops it
  when the turn ends, the camera swings fast, or the player moves. The game's own anim graph plays the turn.
- It reads the camera direction but never writes the camera, so camera mods such as Smoothwalker and
  FreeCam are unaffected.
- It stays out when anything else holds a rotation mode (combat, any higher push), and never turns while
  crouched: the game has no crouched turn for this.
- Untested with `zTBODLocomotionController` (Nexus), which replaces the locomotion layer the turns run through.

## Settings

`config/turninplace.ini`. Saved changes apply in game within a second, except `toggle_key`, which applies
at the next start. A missing file or key falls back to the default below, with one warning in the log.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `enabled` | `1` | `true`/`false`/`1`/`0` | Master switch |
| `turn_angle` | `60` | 46 to 170 | Degrees between camera and facing before a turn |
| `settle_speed` | `60` | 5 to 180 | Camera turning slower than this, in degrees per second, counts as settled |
| `settle_time` | `0.35` | 0 to 2 | Seconds settled before a turn starts |
| `cancel_speed` | `180` | 30 to 360 | A camera swinging faster than this, in degrees per second, stops a turn; never below `settle_speed` |
| `chain_turns` | `1` | `true`/`false`/`1`/`0` | Keep turning while the camera pans slower than `cancel_speed` |
| `toggle_key` | blank | `F1`-`F12`, a letter or digit | Switches the mod on and off; blank is unbound |
| `verbose_log` | `0` | `true`/`false`/`1`/`0` | `1` logs every push and pop with its reason |

### Mod Menu

The page ships as `mod_settings.ini` for Dawnwalker Mod Menu (Nexus mod 271). It is optional and lists every
setting above except `toggle_key`. Apply rewrites the matching numbers in `turninplace.ini`, and the mod
picks the change up within a second. A value hand-edited outside a setting's range above makes the whole
page fail to open.

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
- UE4SS's hot reload (Ctrl+R) restarts the mod on the DLL already loaded: the mod pins its own image so the
  reload cannot crash the game, and no new code is picked up. A new `main.dll` still needs the game closed.

## Layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Superbuild: RE-UE4SS from `DW_RE_UE4SS_SOURCE_DIR`, then the `DWTurnInPlace` target |
| `src/dllmain.cpp` | The DLL: the `AddMovementInput` vtable hook, player discovery, the push and pop state machine on the engine tick |
| `src/config.hpp` | Settings parsing for `turninplace.ini` |
| `mod/` | Exactly what ships under `ue4ss/Mods/DWTurnInPlace/`: `enabled.txt`, `LICENSE`, `config/turninplace.ini`, `mod_settings.ini`; `dlls/` is build output |
| `docs/design.md` | How the game's turn in place works, the measurements behind the design, the plan |

## License

GPL-3.0-or-later. See `LICENSE`.
