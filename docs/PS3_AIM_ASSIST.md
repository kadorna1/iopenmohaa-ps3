# PS3 controller aim assist

The PS3 build has optional aim assist for the DualShock 3 right stick. It is
enabled by default at full strength and is independent of USB-mouse
aiming and menu cursor movement.

## Startup configuration

Each title package carries the same defaults in its own active game directory:

- Allied Assault: `/dev_hdd0/game/OMOHAA020/USRDIR/main/autoexec.cfg`
- Spearhead: `/dev_hdd0/game/OMOHAS020/USRDIR/mainta/autoexec.cfg`
- Breakthrough: `/dev_hdd0/game/OMOHAB020/USRDIR/maintt/autoexec.cfg`

The common `main/autoexec.cfg` is also staged into all three packages. The
expansion-specific copies ensure the defaults are present in the active
`mainta` / `maintt` search path as well, instead of relying on fallback to
the base-game directory. OpenMoHAA loads `autoexec.cfg` after
`omconfig.cfg`, so these values override an older saved profile at startup.
The files are maintained under `assets/ps3_usrdir/main/`,
`assets/ps3_usrdir/mainta/` and `assets/ps3_usrdir/maintt/`. The title-staging
step copies only the matching expansion directory into each expansion package.

The workflow builds a separate executable for each native target game
(Allied Assault = 0, Spearhead = 1, Breakthrough = 2). The shared PS3 input,
game and CGame aim-assist code is compiled into every executable; the
expansion-specific configs provide the same enabled-by-default settings.

## Controls

Tap the DualShock 3 **L2** button during active gameplay to toggle
aim assist. L2 is the PS3 pad's `K_JOY7` input and is detected independently
of its saved key binding, so a customized L2 binding cannot bypass the toggle.
The existing L2 binding still receives its normal key event.

A short **AIM ASSIST ON** or **AIM ASSIST OFF** message appears at the
top-left of the screen for three seconds. The toggle is edge-triggered, so
holding L2 changes the state only once. Aim assist is enabled by default;
the first tap therefore turns it off, and the next tap turns it back on.

Use the in-game console to change the values for the current session:

```
ps3_aim_assist 1
ps3_aim_assist_strength 1.0
```

Set `ps3_aim_assist 0` to disable it. Strength is clamped to 0.0–1.0; zero
disables the effect and one applies the strongest slowdown and pull.

## Behaviour

- Active only during gameplay, with input focus, while the player is turning
  with a controller stick toward a candidate.
- Supports scripted `ET_MODELANIM` soldiers as well as `ET_PLAYER` entities
  in all three single-player titles. The game module publishes its two faction
  values as shared Allies/Axis flags, which lets CGame reject friendly actors
  in Allied Assault, Spearhead and Breakthrough.
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

1. Test a single-player level in each title, including Breakthrough's
   British/Commonwealth Allied soldiers and Axis enemies.
2. Move the right stick toward a living enemy and verify the slight slowdown
   and gentle pull as the reticle approaches.
3. Repeat while aiming at an ally, a corpse, and an enemy behind a solid wall;
   none should attract the reticle.
4. Compare with `ps3_aim_assist 0`, then try strengths such as 0.25 and 0.75.
5. Confirm USB mouse aiming and menu cursor movement remain unchanged.
