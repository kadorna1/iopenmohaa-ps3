# Experimental PS3 RSXKit / SPUKit spike

**Status: experimental, off by default, not a release configuration.**

This branch pins two standalone MIT-licensed libraries as submodules:

- `third_party/rsxkit` — pinned at `a24a976da9e307f91ab210aba56776c158f0ac87`.
- `third_party/spukit` — pinned at `76cd155501c8cb11272268c9a152e96283c17eff`.

## SPU audio mixer

The `OPENMOHAA_PS3_EXPERIMENTAL_SPU_AUDIO` CMake option adds a single persistent SPU worker for the current native PS3 software audio mixer. The PPU snapshots supported PCM/decoded-ADPCM voice state, stages each source's small block window, and submits a 256-frame stereo mix job. The SPU performs sample interpolation, gain ramping, accumulation and clipping, then returns updated cursors. The existing PPU mixer remains the fallback when the worker cannot start, a voice exceeds the bounded staging/pitch limits, or a job reports an error.

This deliberately does **not** move asset decoding, voice creation/retirement, positional gain calculation, the libaudio ring, or the high-level music/streaming APIs to the SPU. The experimental path holds the voice lock during the job to keep source buffers alive; it can increase stalls on the game thread. Benchmark and test it before considering it for normal builds.

To try it, run the Linux GitHub Actions workflow manually and enable **experimental_spu_audio**. The default for push/PR builds is off. Test on real hardware, not just CI. Confirm ordinary effects, looped sounds, one-shots, multiple simultaneous voices, sound stop/start, long samples, and fallback. Compare logs and audible output against an off build.

## Input

Input polling stays on the PPU. PSL1GHT pad and USB keyboard/mouse APIs (`ioPad*`, `ioKb*`, `ioMouse*`) are host I/O calls, not work that can be dispatched as a useful SPU batch. The input event queue is also latency-sensitive and small. Sending those calls or individual events through SPU jobs would add complexity and latency without useful parallelism. No input behavior is changed in this spike.

## Renderer

rsxkit is pinned as an experimental source dependency but **does not replace `ps3gl` in this PR**. It is a custom GCM/RSX drawing API, not an OpenGL 1.1 implementation, and `rsxkit_init()` creates and owns its own GCM context, command buffer, display buffers and frame lifecycle. The current OpenMoHAA renderer already owns those resources in `code/sys/ps3_glimp.c` and translates a large fixed-function GL API through `code/gl/ps3gl_*.c`. Calling rsxkit beside it would create competing RSX ownership; compiling rsxkit and relinking it would not convert the GL API.

A real replacement needs a separate GL1 compatibility adapter for the functions used by the renderer (or a substantial rewrite of renderer draw submission), rendering parity tests, frame/display ownership changes, and an audit of the Cg shader compiler dependency. This branch intentionally leaves the known-working GL1 path as the only renderer to avoid an untestable boot-time regression.

## Validation required before merge

- CI builds the default PPU fallback and can optionally compile the SPU worker.
- Hardware validation is required for audio correctness, underrun behavior, SPU worker startup/shutdown, and measured performance.
- The RSXKit renderer replacement is not implemented by this PR; this document records the integration blocker, not a claim of replacement.
