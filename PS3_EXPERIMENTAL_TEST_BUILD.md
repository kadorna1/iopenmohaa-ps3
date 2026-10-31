# Experimental PS3 test build: ps3gl + SPUKit audio

This branch is a test build for physical PlayStation 3 validation.

- Renderer: keep the existing `ps3gl` renderer active; RSXKit is pinned but is not used to replace it in this build.
- Audio: build the optional SPUKit-backed 256-frame PCM mixer. The PPU mixer remains the fallback for unsupported voices or SPU job failures.
- Input: remains on the PPU; no input behavior change is included.
- Package: install the generated `OpenMoHAA-PS3-OMOHAA020` PKG over the current test install only if you are comfortable replacing that test copy. Keep a known-good PKG available for rollback.

## Please report

1. Whether the XMB lists/opens the title and whether the custom XMB hover sound plays.
2. Whether menu navigation, left/right sticks, D-pad, USB mouse and keyboard still behave as before.
3. Whether ordinary effects play; specifically test one-shots, looped effects, multiple simultaneous sounds, starting/stopping sounds, and a longer sample.
4. Any audio crackle, underrun, delay, stuck sound, or console freeze. Note the scene and approximate time.
5. Whether the existing loading/autosave transition freeze still occurs.
6. Attach the runtime trace/log and note the build stamp at startup.

CI success only confirms compilation, linking and PKG creation; it does not confirm audible correctness or stability on a physical console. Keep this PR experimental until the console test results are reviewed.
