# iOpenMoHAA-PS3

**OpenMoHAA port for the PlayStation 3 (PS3), powered by PSL1GHT.**

iOpenMoHAA-PS3 is a native PlayStation 3 port of [OpenMoHAA](https://github.com/openmoh/openmohaa), targeting the original *Medal of Honor: Allied Assault* and its two expansions, *Spearhead* and *Breakthrough*.

The port adapts the engine to the PS3's PowerPC PPU and RSX architecture using the PSL1GHT development environment. Its platform implementation draws on the PS3 work in ioQuake3 and ioRTCW.

The project focuses on getting the original game and its expansions running natively on PS3, with dedicated platform support, controller integration, audio playback, renderer and game-loop profiling, memory-management improvements and automated multi-title PKG generation.

> **Development status:** Work in progress. The three title variants compile and package through GitHub Actions. Runtime performance, rendering consistency and complete campaign stability are still being tested on physical hardware.

---

## Implemented PS3 features

### 1. Native PS3 platform support

- Dedicated PSL1GHT/PS3DEV cross-compilation toolchain.
- Native PowerPC PPU executable generation.
- PS3 system initialization and shutdown handling.
- RSX/GCM graphics initialization and framebuffer management.
- Triple-buffered display with vertical synchronization.
- PS3 system utility integration, including XMB exit handling.
- Native PS3 filesystem paths and package-specific configuration directories.
- IPv4 socket adaptation.
- PS3 on-screen keyboard (OSK) integration.
- Early boot tracing, runtime logging and PPU exception diagnostics.
- Dedicated PS3 screen logging and diagnostic output.

The PS3 build disables unnecessary desktop-specific dependencies and uses a dedicated platform configuration instead of the standard SDL/OpenAL desktop path.

### 2. RSX graphics and renderer optimizations

The standard PS3 rendering implementation uses the existing OpenGL 1 compatibility renderer (`ps3gl`) backed by the PS3's GCM/RSX interface.

Implemented improvements include:

- Native RSX framebuffer and depth-buffer allocation.
- Dedicated RSX command-buffer and vertex-ring management.
- Triple-buffered rendering and frame synchronization.
- 1280x720 default display resolution, with optional 1080p selection.
- Adjustable screen fitting for displays with overscan.
- Preassembled RSX vertex and fragment shader programs.
- PS3-specific texture allocation, alignment and format handling.
- Native 16-bit texture formats where applicable.
- Optimized lightmap handling.
- Unused renderer image cleanup before texture precaching.

#### Omaha Beach profiling and performance work

The renderer and game module include PS3 profiling for the Omaha Beach workload. The instrumentation separates:

- Renderer command execution, including draw-surface and sprite-surface lists.
- Geometry-builder time, with per-surface-type breakdowns.
- Stretch-picture, miscellaneous backend, swap and `qglFinish` costs.
- Game-frame time for pending events/scripts, actor pre-animation, navigation, bots, active-entity/AI/physics work and post-entity work.

The full 8 MiB vertex ring is allocated in cacheable IO-mapped main memory as two 4 MiB frame segments. It is kept separate from the 4 MiB RSX command buffer. The previous RSX-local vertex-ring path can be selected for A/B testing with:

```sh
PS3GL_FORCE_RSX_VRING=1
```

Skeletal skinning also caches each mesh's global-to-local Tiki bone-index mapping for reuse across vertex weights. Meshes outside the established bone-count bound retain the original lookup path.

These changes target measured renderer and game-module hotspots while preserving surface order, shader behavior, water/HUD rendering and command submission. They are not a claim that Omaha Beach performance is solved: frame rate remains under hardware investigation.

The PS3 renderer now supports a PPU worker-thread backend. Front-end scene construction and back-end drawing use separate, double-buffered frame data; draw commands carry view/frame snapshots so the worker does not read mutable front-end state. `r_smp` defaults to `1` on PS3. If the alternate frame buffer or worker cannot be created, the renderer switches back to the synchronous path. This is PPU renderer concurrency, not renderer work offloaded to an SPE.

The default PS3 renderer configuration uses:

```cfg
seta r_texturebits "16"
seta r_fastdlights "1"
```

These settings reduce renderer memory requirements and avoid unnecessary texture and lighting allocations.

### 3. BSP streaming and memory management

Dedicated PS3 memory optimizations have been implemented to address the console's relatively limited available memory.

These include:

- Streaming BSP surface geometry rather than retaining complete raw vertex and index lumps during conversion.
- Reusable temporary vertex and index buffers sized according to the largest surface being processed.
- Reduced peak memory consumption during map loading.
- Renderer texture-memory accounting.
- Zone allocator diagnostics, including allocation tags and caller information.
- Memory-release reporting when allocator tags are freed.
- Improved renderer image-cache cleanup.
- PS3 save archives are written without the extra LZ77 compression-output buffer; the reader continues to support older compressed saves.
- The PS3 game archive writer now streams through a 64 KiB staging buffer instead of growing a multi-megabyte contiguous allocation in the game zone.
- Archive backpatches retain their random-access semantics: the writer flushes the staging buffer and seeks to update class-pointer counts and object-size fields.
- New saves are written to a `.ps3tmp` file, flushed and closed, then renamed into place only after successful serialization. An incomplete or failed write does not replace the previous valid save.
- The archive field ordering, type tags, version and raw PS3 save format remain unchanged; the existing reader remains compatible.
- Save-archive typed-field mismatch diagnostics report the archive byte offset and current object classname when available. This is diagnostic context only; it does not repair an incompatible or misaligned save.
- CGame checkpoint diagnostics now capture snapshot and emitter-list state. Retained special-effect command events are released before the CGame event allocator is reset, reducing stale event references across teardown/reset boundaries.
- Additional memory diagnostics cover CGame initialization and map transitions.

The streaming writer removes the previously identified large contiguous-allocation requirement. Its interaction with automatic checkpoints, failed writes and save restoration still requires extended physical-console verification.

The BSP streaming implementation specifically targets peak PPU memory consumption during level loading.

### 4. Pre-indexed material shader caches

A PS3-specific material shader cache system is implemented to reduce the cost of loading and indexing the game's material definitions.

The cache avoids repeated shader-file enumeration, concatenation and initial token-level indexing.

Separate caches are supported for all three titles:

| Game | Cache location |
|---|---|
| Allied Assault | `USRDIR/main/scripts/ps3_shader_cache.bin` |
| Spearhead | `USRDIR/mainta/scripts/ps3_shader_cache_mainta.bin` |
| Breakthrough | `USRDIR/maintt/scripts/ps3_shader_cache_maintt.bin` |

The runtime validates the cache format and source-file manifest before using it. If a cache is missing or invalid, the original shader-loading implementation remains available as a fallback.

The caches contain indexed material shader definitions, not preloaded textures or runtime renderer objects. Their main purpose is reducing startup and initial shader-registration overhead rather than increasing steady-state frame rates.

### 5. DualShock 3 and USB input support

A dedicated PS3 input implementation provides:

- Native DualShock 3 button handling.
- Analog left and right stick support.
- Continuous right-stick yaw and pitch input, using the PS3 yaw/pitch axes.
- Radial deadzone compensation and rescaling of the remaining stick travel to the full input range, preserving direction on diagonals and providing a continuous response immediately outside the deadzone.
- Archived `ps3_right_stick_deadzone` calibration cvar (default `12`, clamped to `0–64`) for controller drift and wear.
- D-pad navigation.
- Bindable controller buttons.
- Trigger and pressure-sensitive button handling.
- Menu navigation and automatic directional repeat.
- Left-stick menu pointer movement.
- USB keyboard support.
- USB mouse support, including buttons and scroll wheel.
- Separate mouse sensitivity handling for gameplay and menu navigation.

The default controller mappings are integrated into the engine's normal input and binding systems.

### 6. DualShock 3 aim assist

An optional in-game aim-assist implementation is available for the DualShock 3 right analog stick.

It is enabled by default in all three packages, base game and both expansions.

Default settings:

```cfg
seta ps3_aim_assist "1"
seta ps3_aim_assist_strength "1.0"
```

**Controls**

Tap R1 during active gameplay to toggle aim assist. A short center-print notification appears for three seconds:

- `AIM ASSIST ON`
- `AIM ASSIST OFF`

The toggle is edge-triggered, so holding R1 does not repeatedly change its state.

**Targeting behavior**

- Supports scripted soldier entities and player entities.
- Rejects friendly actors.
- Ignores dead targets.
- Uses a configurable strength between 0.0 and 1.0.
- Evaluates candidates within 48–2048 world units and a 12-degree horizontal / 8-degree vertical aiming cone.
- Tests the three best-aligned candidates against solid world geometry and rejects targets behind walls.
- Applies up to 35% stick slowdown and a small frame-time-scaled angular correction.
- Does not automatically lock onto targets or fire weapons.

The system is implemented in shared PS3 input, game and CGame code and applies to Allied Assault, Spearhead and Breakthrough.

USB mouse aiming and menu navigation remain independent of the aim-assist system.

Documentation: [`docs/PS3_AIM_ASSIST.md`](docs/PS3_AIM_ASSIST.md).

### 7. Native PS3 audio implementation

A native sound driver has been implemented using the PSL1GHT `libaudio` interface.

It replaces the silent PS3 null-audio backend with a software mixer running on the PPU.

Implemented functionality includes:

- Native stereo audio output at 48 kHz.
- PCM sound-effect playback.
- IMA ADPCM decoding.
- Multiple simultaneous sound voices.
- Looping and positional sound effects.
- Stereo panning and distance attenuation.
- Decoder-backed streamed audio.
- WAV, MP3, Vorbis and Opus streaming support.
- Soundtrack music playback.
- Triggered music support.
- RoQ cinematic companion audio.

An optional SPUKit mixer build can also process decoded streamed PCM alongside regular sound voices. It is experimental, disabled in the standard build, and still requires physical-console testing for timing, stutter and channel behavior.

**MP3 playback**

The MP3 codec uses minimp3 instead of the previous libmad implementation.

The PS3 implementation includes additional handling for:

- Stream-buffer refilling.
- MP3 frame synchronization.
- Decoder recovery after invalid frames.
- Corrupted or incomplete input data.
- Stream position and decoder state management.

This addresses the static and white-noise playback problem encountered during earlier PS3 audio testing.

A diagnostic audio test-tone command is also available:

```text
ps3audiotest 1
ps3audiotest 0
```

The first command enables a diagnostic tone, while the second restores normal game audio.

### 8. Independent Allied Assault, Spearhead and Breakthrough packages

The build system produces three independent PS3 executables and packages.

| Game | PS3 TITLE_ID | Target game | Data directory |
|---|---|---:|---|
| Allied Assault | `OMOHAA020` | 0 | `main` |
| Breakthrough | `OMOHAB020` | 2 | `maintt` |
| Spearhead | `OMOHAS020` | 1 | `mainta` |

Each title is compiled separately with its own CMake build configuration, application ID and default target game.

**Expansion data separation**

Each expansion receives its required files in its own package's `USRDIR`:

```text
Breakthrough:
 /dev_hdd0/game/OMOHAB020/USRDIR/maintt/

Spearhead:
 /dev_hdd0/game/OMOHAS020/USRDIR/mainta/
```

The expansion executables can also access shared Allied Assault base-game assets from:

```text
/dev_hdd0/game/OMOHAA020/USRDIR/main/
```

This avoids unnecessarily duplicating the base game's archives in the expansion packages.

**Independent writable directories**

Each title maintains its own configuration, savegames, screenshots, state files and runtime logs under its own application directory.

This prevents the expansions from sharing writable configuration and save data unintentionally.

More information: [`docs/PS3_MULTI_TITLE_PACKAGING.md`](docs/PS3_MULTI_TITLE_PACKAGING.md).

### 9. Automated PKG generation

The repository includes a PS3 packaging script:

`make_ps3_pkg.sh`

It automates:

- ELF stripping.
- SPRX linking.
- EBOOT.BIN generation.
- PARAM.SFO creation.
- PS3 PKG generation.
- Optional GNPDRM package finalization.

When `package_finalize` is available, both standard `.pkg` and finalized `.gnpdrm.pkg` variants can be produced.

The workflow generates packages for all three game titles.

### 10. XMB customization

Optional PlayStation 3 XMB assets can be supplied through:

```text
assets/xmb/
```

The packaging script accepts the contents of this directory without requiring a fixed set of source filenames.

Recognized PS3 assets receive the appropriate handling:

| Filename | Purpose |
|---|---|
| `ICON0.PNG` | XMB application icon |
| `PIC1.PNG` | XMB background |
| `SND0.AT3` | XMB hover sound |

The packaging script handles PNG resizing for the expected PS3 dimensions and validates the ATRAC audio structure before installing `SND0.AT3`.

Other supplied files are copied to the package root unchanged. The directory's `README.md` is excluded from runtime content.

### 11. Automated Linux build system

The repository contains a dedicated GitHub Actions workflow:

`.github/workflows/ps3-build.yml`

The workflow builds the PS3 binaries and packages on Linux using Ubuntu 24.04 runners.

Implemented CI features include:

- Pinned PS3DEV/PSL1GHT Linux toolchain.
- Toolchain checksum verification.
- Persistent host dependency image.
- Cached extracted PS3 development environment.
- Persistent compiler cache using ccache.
- Three independent cached CMake/Ninja build directories.
- Source-hash-based incremental rebuild detection.
- Separate executable linking for all three titles.
- Automated expansion-path and package-staging tests.
- Aim-assist and right-stick radial-response mathematical regression tests.
- Renderer memory and SMP/material-path regression tests.
- Right-stick radial-response and save/archive regression checks.
- Shader-cache validation.
- PKG existence and integrity checks.

Unchanged source files can reuse previously compiled objects across successive workflow executions.

The workflow supports pushes, pull requests and manual execution.

It also supports a manually enabled experimental SPU audio build option, which is disabled by default. The Omaha Beach profiling/vertex-ring pass itself does not move renderer or game simulation work to SPUs; it remains a PPU/RSX optimization.

---

## Repository layout

```text
iopenmohaa-ps3/
│
├── .github/
│   ├── docker/
│   │   └── ps3-builder.Dockerfile
│   └── workflows/
│       └── ps3-build.yml
│
├── assets/
│   ├── ps3_usrdir/
│   │   ├── main/
│   │   │   ├── autoexec.cfg
│   │   │   └── scripts/
│   │   ├── mainta/
│   │   │   ├── autoexec.cfg
│   │   │   └── scripts/
│   │   └── maintt/
│   │       ├── autoexec.cfg
│   │       └── scripts/
│   │
│   └── xmb/
│
├── cmake/
│   └── ps3-psl1ght.cmake
│
├── code/
│   ├── gl/
│   ├── input/
│   ├── renderergl1/
│   ├── sys/
│   └── client/
│
├── docs/
│   ├── PS3_AIM_ASSIST.md
│   ├── PS3_MULTI_TITLE_PACKAGING.md
│   └── PS3_SHADER_CACHE.md
│
├── tests/
│   ├── test_ps3_aim_assist_math.c
│   ├── test_ps3_stick_math.c
│   ├── test_ps3_expansion_loading.py
│   ├── test_ps3_multi_title_config.py
│   ├── test_ps3_renderer_memory.py
│   └── test_ps3_shader_cache.py
│
├── tools/
│   ├── ps3-debug/
│   ├── ps3_shader_cache.py
│   ├── ps3_shader_cache_from_pk3.py
│   └── stage_ps3_title_usrdir.sh
│
├── make_ps3_pkg.sh
└── CMakeLists.txt
```

---

## Changes since v0.1.0.0c

This release candidate consolidates the 118 commits added to `main` after the previous release. The functional updates are grouped below; follow-up commits also add synchronization fixes, regression coverage and CI corrections for these features.

- **Training dialogue and scripted audio:** restore `sounddone` callbacks for PCM and streamed sound-effect voices, assign stable callback channels, and make script sound-state queries check the active mixer voice/stream. Failed sound starts now report a non-playing channel so scripts waiting on dialogue completion can continue.
- **Aim-assist controls:** move the physical toggle to edge-triggered R1, route ON/OFF feedback through the normal center-print UI, and remove the separate CGame overlay. The behavior and regression checks apply to the three title builds.
- **Renderer SMP and frame-state isolation:** add separate front-end/back-end frame storage, double-buffer the scene data, snapshot per-view and frame identifiers for backend work, isolate view-dependent shader/sky/flare state, and dispatch command batches to the PS3 render worker. `r_smp` defaults to enabled on PS3; if worker startup is unavailable, rendering falls back to the synchronous backend.
- **Texture and material handling:** add native RSX uploads for S3TC DXT1, DXT3 and DXT5 textures, support the GL1 `GL_BLEND` texture-environment mode, and render additive multitexture stages in two depth-coherent passes. Dynamic-light scratch data is also separated by frame.
- **Omaha Beach profiling and renderer memory:** add FPS and backend timing reports, direct-XDR versus copied-vertex counters, command/surface-builder and swap/`qglFinish` timings, and per-stage game-frame timings for events/scripts, actor pre-animation, navigation, bots, entities/AI/physics and post-entity work. The vertex ring is 8 MiB in cacheable IO-mapped main memory (two 4 MiB frame segments), kept separate from the 4 MiB RSX command buffer. TIKI skeletal skinning caches each mesh's global-to-local bone-index mapping.
- **DualShock 3 analog response:** replace the abrupt per-axis cutoff with a radial right-stick deadzone and rescale the remaining range while preserving diagonal direction. The archived `ps3_right_stick_deadzone` setting defaults to 12 and is clamped to 0–64; host tests cover continuity, monotonicity, polarity, diagonals and saturation.
- **Save-archive streaming and safer replacement:** stream PS3 saves using a 64 KiB staging buffer, retain random-access archive backpatches with flush/seek, and write to `.ps3tmp` before flushing, closing and renaming a completed save into place. This removes the former large contiguous archive-buffer allocation while preserving archive ordering/version and reader compatibility. Typed-field mismatch logs now include byte offset and object classname where available.
- **CGame teardown and checkpoint diagnostics:** release retained special-effect command events before resetting the event allocator, and add checkpoint snapshot/emitter-list diagnostics to help isolate remaining state or lifetime faults.
- **Build and regression coverage:** expand checks for sound callback routing, R1/center-print behavior, SMP frame isolation, blend/additive material paths, right-stick math, archive behavior, multi-title staging and renderer memory. The PS3 build remains incremental across three independent CMake/Ninja trees and produces standard and GNPDRM packages for Allied Assault, Breakthrough and Spearhead.

The SPUKit streamed-audio mixer is included as an **opt-in experimental build path**; it is not enabled in the standard release configuration. The renderer worker is PPU-side SMP, not SPE renderer offload.

---

## Building

### Requirements

- Linux x86-64.
- PS3DEV and PSL1GHT development environment.
- PowerPC PPU GCC/G++ compiler.
- PS3 RSX/GCM development headers and libraries.
- CMake and Ninja.
- Bison and Flex.
- Python 3.
- Required host development libraries and packaging utilities.

The repository's GitHub Actions workflow prepares the required Linux build dependencies and PS3 toolchain automatically.

### Manual compilation

Export the PS3 toolchain environment:

```bash
export PS3DEV=/path/to/ps3dev
export PSL1GHT="$PS3DEV"
```

Example configuration for Allied Assault:

```bash
cmake -S . -B build-ps3-OMOHAA020 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/ps3-psl1ght.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENMOHAA_PS3_APPID=OMOHAA020 \
  -DOPENMOHAA_PS3_DATA_APPID=OMOHAA020 \
  -DOPENMOHAA_PS3_TARGET_GAME=0 \
  -DBUILD_SERVER=OFF \
  -DBUILD_CLIENT=ON \
  -DBUILD_RENDERER_GL2=OFF \
  -DBUILD_GAME_LIBRARIES=OFF \
  -DBUILD_GAME_QVMS=OFF \
  -DUSE_RENDERER_DLOPEN=OFF \
  -DUSE_OPENAL=OFF \
  -DUSE_HTTP=OFF \
  -DUSE_FREETYPE=OFF \
  -DUSE_INTERNAL_LIBS=ON \
  -DUSE_INTERNAL_SDL=OFF
```

Compile and link the PS3 executable:

```bash
cmake --build build-ps3-OMOHAA020 --parallel 2

cmake --build build-ps3-OMOHAA020 \
  --target ps3_self \
  --parallel 2
```

The resulting executable is:

```text
build-ps3-OMOHAA020/EBOOT.BIN
```

For the expansions, create separate build directories and change the application ID and target game:

| Game | `OPENMOHAA_PS3_APPID` | `OPENMOHAA_PS3_TARGET_GAME` |
|---|---|---:|
| Allied Assault | `OMOHAA020` | 0 |
| Breakthrough | `OMOHAB020` | 2 |
| Spearhead | `OMOHAS020` | 1 |

Keep `OPENMOHAA_PS3_DATA_APPID=OMOHAA020` for all three builds so the expansion executables use the shared Allied Assault data installation.

For normal development, the recommended approach is to use the automated GitHub Actions workflow, which handles all three configurations and their respective packages.

---

## Game data and installation

Original game data is not distributed by this repository.

Supply game data from your own legally acquired copy of Medal of Honor: Allied Assault and its expansions.

The expected base-game installation root is:

```text
/dev_hdd0/game/OMOHAA020/USRDIR/main/
```

Expansion-specific data belongs in (yes, this is intentional):

```text
/dev_hdd0/game/OMOHAA020/USRDIR/maintt/
/dev_hdd0/game/OMOHAA020/USRDIR/mainta/
```

Install the appropriate PKG using the PS3 package installer.

Keep the base Allied Assault installation available when running either expansion.

---

## Testing and current limitations

GitHub Actions validates compilation, executable linking, automated regression tests, package staging and PKG generation.

The current `main` head was validated by [GitHub Actions run #41950564738](https://github.com/kadorna1/iopenmohaa-ps3/actions/runs/41950564738), which completed successfully after the latest renderer/profiler integration fixes.

However, a successful CI build does not guarantee complete gameplay stability on physical hardware.

The following areas remain under active investigation:

- Overall frame rate and gameplay performance, particularly the renderer backend and the active-entity/AI/physics portion of the game frame. PS3 profiling now reports these separately, but the measured Omaha Beach frame rate remains low.
- PPU and RSX memory consumption during extended gameplay.
- Stability during manual saves, automatic checkpoint writes and save restoration. The 64 KiB streaming writer removes the former 8 MiB contiguous-buffer growth path, but the latest checkpoint/save sequence still needs extended testing on physical hardware.
- Save/archive restoration errors. Typed-field mismatches now include an archive offset and current object classname when available; this helps identify where a reader and writer diverge but is not itself a serialization fix.
- Stability when transitioning between levels.
- Expansion loading and rendering consistency.
- BSP streaming behavior on physical hardware.
- Aim-assist behavior across all three campaigns.
- Long-session memory usage and resource cleanup.

The goal is to improve stability and performance on the PS3's limited available system and graphics memory without compromising the original game's behavior.

---

## Experimental development

The normal build retains the `ps3gl` renderer and PPU-based native audio mixer.

An optional SPUKit-backed audio-mixing path is available for experimental builds. It is disabled by default and falls back to PPU mixing when unsupported conditions are encountered.

RSXKit is present as an experimental integration dependency, but it does not replace the current production renderer. A complete renderer replacement would require a separate GL1 compatibility implementation and substantial rendering and lifecycle changes.

Experimental integration notes:

[`PS3_EXPERIMENTAL_RSXKIT_SPUKIT.md`](PS3_EXPERIMENTAL_RSXKIT_SPUKIT.md)

---

## Acknowledgements

- [OpenMoHAA](https://github.com/openmoh/openmohaa) — original engine and game compatibility.
- [ioQuake3-PS3](https://github.com) — PS3 platform and RSX implementation reference.
- ioRTCW-PS3 — additional PS3 platform and rendering reference.
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) — open-source PS3 development environment.
- [PS3DEV](https://github.com/ps3dev) — PS3 development toolchain.

## Disclaimer

This is an independent, community-developed port. It is not affiliated with or endorsed by Electronic Arts or the original Medal of Honor developers.

All original game assets must be obtained and supplied by the user.

Link to the original readme for the openmohaa project, with stuff that still isn't implemented, can be found in https://github.com/kadorna1/iopenmohaa-ps3/blob/main/README_ORIGINAL.md
