# PS3 controller aim assist

The PS3 build has optional aim assist for the DualShock 3 right stick. It is
enabled by default at a moderate strength and is independent of USB-mouse
aiming and menu cursor movement.

## Controls

Use the in-game console:

```
ps3_aim_assist 1
ps3_aim_assist_strength 0.50
```

Set `ps3_aim_assist 0` to disable it. Strength is clamped to 0.0–1.0; zero
disables the effect and one applies the strongest slowdown and pull.

## Behaviour

- Active only during gameplay, with input focus, while the player is turning
  with a controller stick toward a candidate.
- Supports Allied Assault's scripted `ET_MODELANIM` soldiers as well as
  `ET_PLAYER` entities. Opposing Allies/Axis flags are used to reject allies.
- Dead actors and players are ignored. Scripted actors publish the shared
  `EF_DEAD` flag on death.
- Candidates must be between 48 and 2048 world units away and within an
  elliptical aim cone (12 degrees horizontal, 8 degrees vertical).
- The three best-aligned candidates are tested against solid world geometry;
  targets behind walls are rejected.
- The effect combines up to 35% stick slowdown with a small, frame-time-scaled
  angular pull. It is not a lock-on and does not fire weapons.
- USB mouse aiming, keyboard input, and menu navigation are unchanged.

The cone math has host-side unit tests in
`tests/test_ps3_aim_assist_math.c`, run before the PS3 build in CI.

## Hardware test checklist

1. Load a single-player level containing Allied and Axis soldiers.
2. Move the right stick toward a living enemy and verify the slight slowdown
   and gentle pull as the reticle approaches.
3. Repeat while aiming at an ally, a corpse, and an enemy behind a solid wall;
   none should attract the reticle.
4. Compare with `ps3_aim_assist 0`, then try strengths such as 0.25 and 0.75.
5. Confirm USB mouse aiming and menu cursor movement remain unchanged.
