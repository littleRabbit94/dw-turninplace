# Design

## The game already has turn in place

The player's locomotion is `RebelLocomotion` (Rebel Wolves' own framework). Its turn in place is
complete and loaded for the player; exploration just never feeds it.

| Piece | Value (measured 2026-09-23, vanilla) |
|---|---|
| Movement component | `DWPlayerMovementComponent` (`URebelCharacterMovement`) |
| Exploration rotation mode | `1` = `FaceVelocity` (orient to movement), rotation sync `0` = `AnimDriven` |
| Movement profile | `DA_OpenWorld_MovementProfile`: `TurnInPlaceYawOffset` 45, `AimYawMax` 60 |
| Anim blueprint | `ABP_Character_Main_C`, linked layers `ABP_HumanLocomotionDefaultLayers_C`, `ABP_HumanControlRigLayers_Male_C` |
| Turn animations | `Coen_Turn_45/90/180 L/R`, set `DA_FaceDirection_TurnInPlaceBlendSpaceSet` |
| Entry condition | `DA_IdleToTurnInPlace_Condition`: `FaceTargetAngle` non-zero, `RemainingTurnAngleWeight` 0, `RootYawSpeedWeight` below 1, not `WantsToStop` |

`FaceTargetAngle` stays 0 in `FaceVelocity`. Pushing rotation mode `2` (`FaceDirection`) through
`PushRotationMode` makes the game compute it from the camera, and past 45° (the profile's
`TurnInPlaceYawOffset`) the turn state machine plays a turn. Below that only the head follows (±60°).
`FRangeBound` types in the condition assets: 0 exclusive, 1 inclusive, 2 open.

## How a turn moves the character

Actor based, not mesh offset: the mesh stays locked to the actor for the whole turn (relative yaw -90,
root bone on the mesh). The actor rotates by the animation's root yaw speed curve (`AnimDriven` sync).

| 90° turn, sampled every 33 ms | |
|---|---|
| Actor rotation | 0.25 s to 0.93 s, peak about 192°/s |
| Turn weight held | to about 1.2 s (feet settle) |
| Back to idle | about 1.6 s |
| Pop mid turn, no input | the turn winds down in about 0.24 s, about 20° of carry-over, no snap |
| Camera swung back mid turn | the turn re-targets |

This is the technique Vaei's `TurnInPlace` plugin (MIT) adds to projects that lack it; the game has it
natively, so that plugin is a reference only (it also needs UE 5.8 and subclassing, the game is 5.5.4).

## The handover problem

`FaceDirection` must be popped before the player moves, or the start is a strafe start. Lua probes
(33 ms poll) showed:

- Pushed for the whole idle: starts sometimes began before the pop, then stalled 150 to 200 ms.
- Pushed only while a turn is needed (settle trigger): clean starts, except a walk-off mid turn, where
  the turn animation's own step and its root yaw carry-over show as about 70 ms off-axis before the
  start takes over.
- Vanilla keyboard starts (baseline): speed 0 for about 0.1 s, then a smooth ramp; no dip.

A 33 ms poll cannot pop on the input frame. Hence the native hook.

## The input hook

`APawn::AddMovementInput` is virtual. Its exec thunk (`execAddMovementInput`, +0x52A1A00 on the
2026-09-10 build) ends in `call qword ptr [rax+858h]`: slot 267. The DLL reads the slot out of the thunk
at load (the last `call [reg+disp]` before the `ret`), so a rebuild of the game that moves the slot is
followed, and cross-checks 267. `DawnwalkerPlayerCharacter` overrides the function; the hook sits in that
class's vtable, reached through `Default__DawnwalkerPlayerCharacter` (Blueprint subclasses share it).

Phase 0 (spike) results, 7 walk-offs:

| Question | Result |
|---|---|
| Does input reach the virtual? | Yes, 2 calls per input frame, no frame with acceleration and no hooked input |
| Before the movement tick? | Yes: acceleration 0 inside the hook, 2000 at the end of the same frame |
| Does a pop from the hook land that frame? | Yes: mode 2 right after the pop, 1 at the end of the frame (the stack resolves in the movement tick) |

## Phase 1 live run (measured 2026-09-23, vanilla, keyboard)

| | Result |
|---|---|
| Pushes | 16, every one produced a turn; no `no_turn`, `override`, `left_idle` or `safety` pops |
| Push delay | 0.30 to 0.35 s after the camera stopped; longer where it crept before stopping |
| Pops | 3 `done` (1.5 to 2.1 s after the push), 13 `input` (8 after the rotation ended, 5 mid turn) |
| `bIsPlayingRootMotion` during a turn | false in all 10 samples, `bIsOnGround` true: safe as a pre-push guard |
| Swing and go, walk-offs | no turn on swing and go; walk-offs clean (player report) |

A turn near 180° that re-targets to the other side reads as over (`TurnInPlaceAngle` 0, idle) for one
tick. "Turn finished" therefore also needs `RemainingTurnAngle` 0 and must hold for 0.1 s, or
`chain_turns = false` would cut such a turn off.

## The wait applies to every turn (2026-09-23)

While `FaceDirection` is pushed, the game re-targets the turn to the camera at once whenever the camera is
past its own 45°: no wait. A player report ("turns almost immediately when turning the mouse quickly, even at
a 1 s wait") matched the log: every push waited the full `settle_time`, but holds lasted 3 to 16 s and quick
swings inside them turned at once. A fast camera now drops the push (`pop camera`); the turn winds down
without a snap and the next waits again.

One threshold for both start and cancel failed at `settle_speed` 60: per-frame mouse deltas spike above a
slow pan's average, so a pan near the threshold started and cancelled a turn every few frames (holds of
0.01 to 0.1 s in the log). The camera speed is now smoothed (0.1 s time constant) and has hysteresis: a
turn starts below `settle_speed` held for `settle_time`, and is cancelled only above `cancel_speed`
(never below `settle_speed`). A fixed 3 x `settle_speed` (90) left no start-stop churn
(shortest hold 0.36 s over 22 turns), but the player's deliberate cancel swings measured 91 to 148 deg/s
smoothed and their natural pans ran above 90, so the follow window felt too slow: hence its own setting.
Between the two, a held turn keeps following a pan. Circling the character smoothly with a mouse is
awkward (player report), so pan following is a side benefit, mostly for a controller stick.

Shipped defaults from the player's tuning (0.2.0): `turn_angle` 60, `settle_time` 0.35 s, `settle_speed`
60 deg/s, `cancel_speed` 180 deg/s. With smoothing, raising `settle_speed` alone to 60 (cancel at 3 x = 180)
already felt right; `cancel_speed` stays a setting so the two can be tuned apart.

The camera does not follow the actor during a turn (control yaw constant over a 2.4 s measured turn), so a
turn cannot cancel itself.

## Crouch and combat (measured 2026-09-23)

Crouched: `bIsIdle` stays false standing still, before and during a turn. A forced `FaceDirection` push
while crouched does rotate the actor (0.19 to 0.9 s, peak 200°/s), but the anim graph plays the standing
turn from the crouch pose: the player saw a snap to a standing pose. The mod never pushes while
crouched and pops if the player crouches mid turn; there is no setting for it.

Combat (weapon drawn, standing): the game pushes `DA_Combat_MovementProfile` (priority 1, config rotation
mode 2, `AimYawMax` 90) and its own rotation entry (mode 2, priority 1), and links
`ABP_CoenVampire_Combat_C`. Its turns in combat are the game's own. The mod's push guard (stack holds only
the base entry, mode `FaceVelocity`) keeps it out; a draw during a held turn grows the stack to 3 and pops
it as `override`.

## Other facts the code relies on

- Find the pawn through the player controller's `Pawn`. `FindFirstOf("DawnwalkerPlayerCharacter")` can
  return a cutscene spawnable template (`CS004_wakeUp_part2_LS`) with no anim instance.
- The rotation stack only resolves on a movement tick, so nothing changes while the game is paused.
- UE4SS keybind callbacks run off the game thread: they raise a flag the engine tick acts on.
- `zTBODLocomotionController` (a Nexus pak mod) replaces `ABP_HumanLocomotionDefaultLayers`,
  `DA_OpenWorld_MovementProfile` and the speed up and slow down conditions. Compatibility with it is
  untested.

## Plan

| Phase | Work |
|---|---|
| 0, done | Spike: the hook, same-frame pop proven |
| 1, done (tested live 2026-09-23, see below) | Settle trigger (camera past 50° and turning under about 30°/s for 0.3 s), chaining while panning, done when a seen turn ends, pop in the hook, guards (pause, idle and walking only, another system's rotation mode, crouch), ini |
| 2, in progress | Test matrix. Done: combat, crouch, cutscene, dialogue, save and load. Wolf form is a sprint-only ability in vanilla, so the idle guards keep the mod out of it. Open: torch, horse (if any), map changes |
| 3, in progress | Done: Mod Menu page (0.2.0), camera speed smoothing and `cancel_speed`, tuned defaults. Open: `zTBODLocomotionController` compatibility, release packaging; mid-turn polish only if wanted |
