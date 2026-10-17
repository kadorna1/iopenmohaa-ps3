# OpenMoHAA PS3 / PSL1GHT — clean source

This is the cleaned OpenMoHAA-PS3 source tree with the PS3 port changes through cycle5 applied.
It contains source, CMake files, assets and third-party sources needed by the project, but no build
outputs, synthetic SDK, fake compiler fixtures, or WIP logs.

## Required environment

Use a real PS3 development environment with PSL1GHT/PS3DEV installed. The toolchain file expects
these tools under `$PS3DEV/ppu/bin/`:

- `ppu-gcc`
- `ppu-g++`
- `ppu-ar`
- `ppu-ranlib`
- `ppu-strip`
- `ppu-ld`
- `ppu-nm`
- `ppu-objcopy`
- `sprxlinker`
- `fself`

You also need working host `bison` and `flex`, plus the PS3 RSX/GCM headers and libraries supplied
by your PSL1GHT/PS3DEV installation.

Before configuring, source your normal PS3DEV/PSL1GHT environment so `$PS3DEV` is exported.

## Configure

From the root of this directory:

    rm -rf build-ps3

cmake -S . -B build-ps3 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/ps3-psl1ght.cmake \
      -DBUILD_SERVER=OFF

Then:

    cmake --build build-ps3 -j

The PS3 packaging target is:

    cmake --build build-ps3 --target ps3_self

Its configured output is `EBOOT.BIN`, following the PSL1GHT strip -> sprxlinker -> fself flow.

## Current PS3 port state

- PS3 uses the null sound driver (`S_PS3NULL_*`) until a native CELL audio backend is linked.
- Legacy GameSpy GCD/QR2/netex is excluded from the PS3 client/server build boundary.
- The PS3 server-list UI uses the in-tree compatibility layer instead of the legacy GameSpy socket API.
- Game and cgame modules are assembled through static archives and GNU ld `--whole-archive` partial links.
- Parser sources are generated at configure/build time by Bison/Flex; generated parser output is not
  treated as a checked-in production artifact.
- RSX initialization/frame lifecycle, PS3 input/OSK, sysutil/XMB handling and IPv4 socket adaptation
  are included in the port.

## What has not been verified here

This source was developed and checked with an isolated synthetic toolchain/SDK environment because the
originating container did not contain the real PSL1GHT PPU toolchain or PS3 hardware/emulator. The first
real PS3 build may therefore expose PPC ABI, linker relocation, RSX SDK-version, or SELF-layout issues;
those are intentionally not hidden by shipping fake binaries.


### GCC 7.2 note
The bundled PS3 toolchain selects GNU C11 for C sources because the PSL1GHT GCC 7.2 compiler rejects `-std=gnu17`. C++ remains on GNU C++17.


## Packaging and debugging (v50)

See `SNAPSHOT_README_v50.md` for the current state, `make_ps3_pkg.sh` for building an installable
PKG from the linked ELF, and `tools/ps3-debug/` for the probe programs and the UDP log listener.
The statement above that this source was never built on the real toolchain is out of date: it has
been compiled and linked with the PSL1GHT GCC 7.2.0 toolchain and run on a console as far as the
first frame.
