# PS3 pre-indexed material shader cache

The PS3 GL1 renderer already loads preassembled RSX vertex/fragment programs
from `code/gl/ps3gl_shader_data.h`. This optional cache addresses a different
cost: startup enumeration, loading, concatenation, compression and token-level
indexing of the game's `scripts/*.shader` material definitions.

## Generate directly from the game's PK3 archives (recommended)

No `main/scripts` directory or manual extraction is required. The helper reads
the original `.pk3` archives in your Allied Assault `main` directory and
extracts only the direct `scripts/*.shader` entries into a temporary folder.
It does not use `scriptcache/*.psc` files.

From the PR branch, run:

```sh
python3 tools/ps3_shader_cache_from_pk3.py \
  --main-dir "/path/to/Allied Assault/main" \
  --output "/tmp/ps3_shader_cache.bin"
```

The helper follows the engine's archive priority: PK3 names are sorted
case-insensitively and later names override earlier ones; loose files in
`main/scripts`, when present, override archive entries. It removes duplicate
filenames the same way the engine's filesystem listing does, then generates
the indexed cache. The output reports archive count, unique source-file count,
definition count and duplicate overrides.

Copy the generated `ps3_shader_cache.bin` to a loose `scripts/` directory
visible to the PS3 game filesystem, for example:

```text
/dev_hdd0/game/OMOHAA020/USRDIR/main/scripts/ps3_shader_cache.bin
```

Keep this generated file with your own game data. It contains shader text
derived from the original game archives and is not included in the repository
or PKG releases.

## Generate from an already extracted scripts directory

If you already have a directory containing the direct `*.shader` source files,
the lower-level generator is also available:

```sh
python3 tools/ps3_shader_cache.py \
  --scripts-dir "/path/to/extracted/main/scripts" \
  --output "/tmp/ps3_shader_cache.bin"
```

## Runtime behavior

On PS3, `R_StartupShaders()` looks for `scripts/ps3_shader_cache.bin`.
The loader checks the format version, record sizes, all offsets/terminators,
and the sorted shader-file-name manifest before using it. A missing cache is
normal. An invalid cache or a different set of shader filenames causes the
renderer to use the original script scanner and parser.

The cache stores shader names and original shader-definition text blocks, not
renderer structs, image pointers, lightmap pointers, or RSX programs. The
existing `ParseShader()` / `FinishShader()` path remains responsible for
resolving textures, lightmaps, runtime state and material behavior.

**Regenerate after editing or replacing any `.shader` file**, even when its
filename remains the same. The runtime can detect added/removed/renamed shader
files, but intentionally does not reread every source file just to validate its
contents; doing so would erase much of the I/O benefit.

## Scope and expected benefit

The cache avoids per-file reads, the temporary file-buffer array, the large
`strcat` concatenation, `COM_Compress()`, and the startup token scan used to
build the material-name hash table. Material definitions are still parsed
when first requested, and texture loading remains unchanged. This is therefore
a startup/first-registration optimization, not a promise of higher steady-state
FPS. Use the normal fallback for modded or changed script sets by regenerating
the cache or removing it.

The tools use only Python's standard library. Parser, binary layout and PK3
archive priority behavior are covered by `tests/test_ps3_shader_cache.py`.
