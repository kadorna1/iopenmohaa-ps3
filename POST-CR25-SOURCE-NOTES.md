# OpenMoHAA PS3 — clean post-CR25 source tree

Source baseline: retained OpenMoHAA v50 training/precache patched source.
Snapshot date: 2026-10-17.
Target: PS3 CFW/HEN, TITLE_ID OMOHAA020.

This archive is source-only. It does not contain generated build directories,
ELF/SELF binaries, PKG files, or commercial game data/PK3 assets. It includes
the source tree with the relevant changes applied directly to source files;
there are no patch files required to apply these changes after extraction.

## Included post-CR25 source changes

1. Retains training-precache and entity-spawn tracing from the previous patch stack.
2. `code/fgame/level.cpp`: on PS3, avoids eagerly compiling every animation
   script during single-player level precache; animation scripts compile on
   first use.
3. `code/script/scriptopcodes.cpp`: opcode lengths for `OPCODE_SWITCH` and
   `OPCODE_LOAD_CONST_ARRAY1` match their pointer/operand sizes.
4. `code/script/scriptcompiler.cpp`: persistent script-cache version is 4;
   persistence is skipped for bytecode containing process-local data or invalid
   opcode layouts, with trace diagnostics.
5. `code/fgame/archive.cpp`: `Archiver::FileError()` captures read/write mode
   and filename before closing the archive so diagnostics retain their context.

## Build notes

Use `BUILD_PS3.md` and `cmake/ps3-psl1ght.cmake` for the PS3 build configuration.
The generic `make_ps3_pkg.sh` packaging helper is retained. Bison and Flex must
be available for parser generation. A successful compile/package does not, by
itself, establish a successful runtime test on physical PS3 hardware.

On first launch after moving from cache version 3, the engine should reject old
cache entries and regenerate them under version 4. Keep the existing licensed
MOHAA data under `/dev_hdd0/game/OMOHAA020/USRDIR/main`; it is not included here.
