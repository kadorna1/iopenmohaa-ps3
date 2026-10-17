# OpenMoHAA PS3 / PSL1GHT WIP

This tree is the working PS3 port. It uses the ioQuake3-PS3 and ioRTCW-PS3
implementations as platform/RSX references and keeps OpenMoHAA's engine code
as the primary source.

Current milestone: platform and GL1/RSX scaffolding.

Target title: Allied Assault / War Chest (main game data supplied separately).

## Build state

The PS3 CMake path is intentionally cross-only: desktop SDL/OpenAL/HTTP loaders and Unix platform sources are excluded. Game and cgame are compiled as relocatable PS3 objects and attached to the client EBOOT after their private symbols are localized. A real PSL1GHT build still requires the PS3DEV toolchain and SDK; host-side configure tests do not validate PS3 ABI or RSX execution.
