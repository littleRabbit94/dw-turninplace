# Nexus page metadata for Turn In Place

Game page: https://www.nexusmods.com/thebloodofdawnwalker (game id 9719, domain
`thebloodofdawnwalker`). Source: `src/` (DLL) and `mod/` (mod folder).
Archive: `Build-Package.py` (version from `ModVersion` in `dllmain.cpp`).

Not posted yet. Nexus strips backslashes in the Files-tab description; use forward slashes there.

Sibling pages, same author: Smoothwalker - Third Person Camera (mod 613, category Utilities, tags
Gameplay, Utilities for Modders, Utilities for Players, Overhaul, Camera, Quality of Life, AI Assisted)
and FreeCam - UE4SS (mod 350, Utilities). Read 2026-09-23. No other page on this game has "Turn" or
"Locomotion" in its name (searched 2026-09-23).

## Page fields

| Field | Value |
|---|---|
| Mod name | `Turn In Place` |
| Summary | `Standing still, your character turns on the spot to face the camera with the game's own turn animations, once the camera settles. Walking off starts a normal walk or run. Settings in an ini or on a Mod Menu page. Needs UE4SS and Steam build 25232147.` |
| Version | `0.2.1` |
| Category | `Gameplay` |
| Tags | `Animation`, `Gameplay`, `Quality of Life`, `AI Assisted` |
| Adult content | No |
| Requirements | Two entries, either one. UE4SS for Dawnwalker: https://www.nexusmods.com/thebloodofdawnwalker/mods/18 (Vercadi). Note field: `Or Framecore's UE4SS for BoD. Use file Dawnwalker-UE4SS-v1.2.1-rc6-build25232147 (version 1.3).` UE4SS for BoD: https://www.nexusmods.com/thebloodofdawnwalker/mods/283 (Framecore). Note field: `Or Vercadi's UE4SS for Dawnwalker. Version 2c or 2b, Performance or Compatibility profile.` Both carry UE4SS `97b7e501`, the commit the DLL is built against. Tested 2026-09-23 on Vercadi rc6 and on Framecore 2c, both profiles (see Before upload). |
| Optional | Dawnwalker Mod Menu: https://www.nexusmods.com/thebloodofdawnwalker/mods/271 (Nexus title "Mod Setting Menu", mmarcussa, 1.0.7 on 2026-09-23). Note field: `Adds the in-game settings page. 1.0.5 or later hides the turning settings while the mod is off; tested on 1.0.7. Without it, edit config/turninplace.ini.` |
| Description | `nexus-description.bbcode` |

## Files tab

| Field | Value |
|---|---|
| File name | `Turn In Place` |
| Version | `0.2.1` |
| Category | Main Files |
| File | `dist/TurnInPlace-0.2.1.zip`, 193,419 bytes, built 2026-09-23 |
| Description | `Close the game, then extract into the folder that holds the ue4ss folder (Dawnwalker/Binaries/Win64, the one with Dawnwalker.exe). The archive carries the folder path, so the mod lands in ue4ss/Mods/DWTurnInPlace by itself. Needs UE4SS (mod 18 or mod 283) and Steam build 25232147. No mods.txt edit.` |

Contents: `dlls/main.dll` (no PDB), `mod_settings.ini`, `config/turninplace.ini`, empty `enabled.txt`,
`LICENSE`. No exe or installer.

## Permissions

| Question | Answer |
|---|---|
| Others can use assets without permission | Yes |
| Others can upload as-is / convert / modify | Yes, with credit |
| Asset use in mods you sell | No |
| Donation Points | Decline |

Credit field: `GPL-3.0-or-later. Built on UE4SS by the UE4SS-RE team (MIT). Inspired by Vaei's TurnInPlace (MIT). Runs on Vercadi's and Framecore's UE4SS packages for this game.`

## Licensing (checked 2026-09-23)

| Shipped | License |
|---|---|
| `main.dll` | GPL-3.0-or-later (own code). Links `UE4SS.dll` (RE-UE4SS, MIT) at run time; its other imports are the MSVC and Windows runtimes, which the player already has and the archive does not carry. No build path inside (`/PDBALTPATH:main.pdb`). |
| `mod_settings.ini`, `turninplace.ini`, `enabled.txt` | Own text, GPL-3.0-or-later. `mod_settings.ini` follows the file format of mmarcussa's Mod Setting Menu; no code from it. |
| `LICENSE` | GPL-3.0 text, unchanged |
| Page art | Header and thumbnail from own captures; Coen cut out with rembg. Fonts Anton, Jost and Cinzel Decorative, SIL OFL. The captions reference Zoolander ("ambi-turner") and "Total Eclipse of the Heart" ("Turn around") as text only; no stills, no lyrics. Details in `art/NOTES.md`. |
| Credited, not shipped | Vaei's [TurnInPlace](https://github.com/Vaei/TurnInPlace), MIT (checked 2026-09-23), as inspiration. No code from it. |

## Images

| Image | File | Uploaded URL |
|---|---|---|
| Page header, 1300x372 | `dist/header-1300x372-turnaround.png` | (fill in) |
| Thumbnail, 1600x900 | `dist/thumbnail-ambiturner-im.png` | (fill in) |
| Gallery | Wanted: a before and after of a camera swing while standing (head only, then the turn) | |

Nexus's header upload crops to 1300x372. How the two images were built (sources, fonts and their
licenses, positions) is in `art/NOTES.md`, which is not tracked, like the rest of `art/` and `dist/`.
`art/make_header.py` and `art/make_thumbnails.py` rebuild them.

## Before upload

- Framecore's UE4SS for BoD (mod 283): done 2026-09-23 on 2c, passed on both profiles. Its `UE4SS.dll`
  (stock 97b7e501, banner `Git SHA #97b7e501`) with no `VTableLayout.ini` or `UE4SS_Signatures/`, shipped
  defaults. Both: v0.2.0 load line, slot 267 hooked, player found, no errors. Performance: 9 pushes
  (settled 0.35 to 0.45 s), pops 4 done, 4 camera, 3 input; live ini reload worked. Compatibility
  (`profile_compat.ini` over the settings, as the switcher does): 7 pushes, pops 1 done, 3 camera,
  3 input. Turns and walk-offs clean by eye. 2b ships the same DLL, so it is covered.
- Save check: done 2026-09-23, passed. The mod's `FaceDirection` entry (priority 1, mode 2, handle 1)
  was on `RotationModeStack` with the pause menu open; a manual save (`ManualSave2.sav`) was written
  then. Loaded with the mod off (no `enabled.txt`, no log lines), the stack held only the game's base
  entry (priority 0, mode 1, handle 0), `CurrentRotationMode` 1, and the walk-off was a normal start.
  The player persists through a population stub (`ActorStubComponent` to `DogwoodPlayerAIStub`), not
  the movement component. Saves are compressed, so a byte scan proves nothing either way. The pause
  menu does not pop the entry; the save simply does not keep it. On disk the mod writes only
  `config/turninplace.ini`.

## Changelog tab

Plain text, no BBCode. `Build-Package.py` reads the first version in this block and fails if it
differs from `ModVersion` and `[Mod] Version`: add the new entry on top.

```
0.2.1 - Hot reload fix

- UE4SS hot reload (Ctrl+R) no longer crashes the game: the mod keeps its DLL loaded and restarts on it. Note that a reload never picks up a new main.dll; close the game for that.

0.2.0 - First release

- Standing still, your character turns on the spot to face the camera once it has swung past the turn angle and settled, using the game's own turn animations.
- Quick glances and swing-and-walk never turn. Walking off hands back to the game on the same frame, so starts are the game's normal walk and run starts.
- A fast camera swing stops a turn; a slow pan keeps your character following.
- Stays out of combat (the game's own combat turns run), crouch, cutscenes and dialogue.
- Settings in config/turninplace.ini, re-read while the game runs. Optional Dawnwalker Mod Menu page with live Apply. Optional toggle key, unbound by default.
- Defaults: turn angle 60 degrees, wait 0.35 s, camera counts as stopped below 60 deg/s, a swing above 180 deg/s stops a turn, keep turning while panning on.
- Built for UE4SS 97b7e501: Vercadi's rc6 (mod 18) or Framecore's 2c or 2b (mod 283). Steam build 25232147.
```

## Maintenance

- Version: `ModVersion` in `dllmain.cpp`. It also appears in `[Mod] Version` in `mod_settings.ini`
  and the newest changelog entry above (the build checks all three), and in places the build does not
  check: the description's log-line `[code]` block and the version rows in the tables above.
- `Build-Package.py` fails when a `ConfigKey` in `mod_settings.ini` is missing from `turninplace.ini`
  or appears twice, or a shipped value is outside `Minimum`/`Maximum`, off its `Step` or not in
  `PresetValues`. Any of those stops the Mod Menu page opening at all, not just one row.
- It also fails when `main.dll` is older than the newest file in `src/`. A `git checkout` touches
  source mtimes, so after switching branches rebuild before packaging.
- The description lists the shipped defaults by hand; after changing a default in `turninplace.ini`,
  `mod_settings.ini` `Default =` and `config.hpp`, update it and the changelog.
- Wording: "rotation mode", never "camera mode". The mod reads the camera and never writes it.
- Voice: first person where it helps, short, a hobbyist engineer explaining what it does. No sales
  language, no em dashes, no AI credit line in the text (the tag carries the disclosure).
- The description's Source section links the GitHub repo, public since 2026-09-23. Sweep commit
  messages and comments for personal info before each push.
- Game build: the DLL reads the `AddMovementInput` vtable slot out of `execAddMovementInput` at load and
  cross-checks 267 (`docs/design.md`, "The input hook"). If it cannot find the call it logs an error and
  stays inactive. A game patch means retesting and updating the build number here and on the page.
- Loader: a new UE4SS release on mod 18 or mod 283 means checking its UE4SS commit, rebuilding against
  it and retesting, then updating the Requirements rows and the description. The DLL links `UE4SS.dll`,
  so a commit mismatch can fail to load.
- Untested at release: the torch and `zTBODLocomotionController`, the pak from Controller and
  Keyboard True Analog Movement (mod 493, r457), which replaces the locomotion layer the turns use.
  Move each to Compatibility once tested.
- The config lives in `config/`, not `scripts/config/`: UE4SS starts a Lua mod for any mod folder with a
  `scripts` subfolder and logs a red "main.lua not found" error. Do not add a `scripts` folder.
- Log lines in the description are inside `[code]`: `[DWTurnInPlace]` outside one parses as a BBCode tag.
