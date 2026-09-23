# Turn In Place

A turn-in-place mod for The Blood of Dawnwalker. When Coen stands still and the camera swings away from
where he faces, he turns on the spot with the game's own turn animations instead of only turning his head.

Status: in development. The current build is the phase 0 spike: it hooks movement input and logs, and
`F7` pushes the game's camera-facing rotation mode for one pop test. It does not turn the character yet.

## Requirements

- The Blood of Dawnwalker, Steam build 1.0.5 (25232147).
- UE4SS at commit `97b7e501`, from either:
  - Vercadi's UE4SS for Dawnwalker, rc6 (Nexus mod 18), or
  - Framecore's UE4SS for BoD, 2c or 2b, Performance or Compatibility profile (Nexus mod 283).

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
`--parallel`, not `-- /m`: Git Bash rewrites `/m` into a path. To deploy, close the game and copy `mod/`
to `Dawnwalker/Binaries/Win64/ue4ss/Mods/DWTurnInPlace/`.

## Layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Superbuild: RE-UE4SS from `DW_RE_UE4SS_SOURCE_DIR`, then the `DWTurnInPlace` target |
| `src/dllmain.cpp` | The DLL: the `AddMovementInput` vtable hook, player discovery, the engine tick |
| `mod/` | Exactly what ships under `ue4ss/Mods/DWTurnInPlace/`: `enabled.txt`, `LICENSE`; `dlls/` is build output |
| `docs/design.md` | How the game's turn in place works, the measurements behind the design, the plan |

## License

GPL-3.0-or-later. See `LICENSE`.
