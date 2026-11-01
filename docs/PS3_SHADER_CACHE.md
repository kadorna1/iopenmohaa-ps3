# PS3 pre-indexed material shader cache

The PS3 GL1 renderer already loads preassembled RSX vertex/fragment programs
from \`code/gl/ps3gl_shader_data.h\`. This optional cache addresses a different
cost: startup enumeration, loading, concatenation, compression and token-level
indexing of the game's \`scripts/*.shader\` material definitions.

## Generate the cache on a PC

Extract the \`scripts/*.shader\` files from the exact game data set you use into
one directory, preserving the filenames. Then run:

\`\`\`sh
python3 tools/ps3_shader_cache.py \\
  --scripts-dir "/path/to/extracted/main/scripts" \\
  --output "/path/to/extracted/main/scripts/ps3_shader_cache.bin"
\`\`\`

Copy \`ps3_shader_cache.bin\` into a loose \`scripts/\` directory visible to the
game filesystem (for example, alongside the extracted scripts under the game's
\`main\` search path). Do not put the cache inside the source repository or
distribute it with the port: it contains material-script text derived from the
user's game assets.

## Runtime behavior

On PS3, \`R_StartupShaders()\` looks for
\`scripts/ps3_shader_cache.bin\`. The loader checks the format version, record
sizes, all offsets/terminators, and the sorted shader-file-name manifest before
using it. A missing cache is normal. An invalid cache or a different set of
shader filenames causes the renderer to use the original script scanner and
parser.

The cache stores shader names and original shader-definition text blocks, not
renderer structs, image pointers, lightmap pointers, or RSX programs. The
existing \`ParseShader()\` / \`FinishShader()\` path remains responsible for
resolving textures, lightmaps, runtime state and material behavior.

**Regenerate after editing or replacing any \`.shader\` file**, even when its
filename remains the same. The runtime can detect added/removed/renamed shader
files, but intentionally does not reread every source file just to validate its
contents; doing so would erase much of the I/O benefit.

## Scope and expected benefit

The cache avoids per-file reads, the temporary file-buffer array, the large
\`strcat\` concatenation, \`COM_Compress()\`, and the startup token scan used to
build the material-name hash table. Material definitions are still parsed when
first requested, and texture loading remains unchanged. This is therefore a
startup/first-registration optimization, not a promise of higher steady-state
FPS. Use the normal fallback for modded or changed script sets by regenerating
the cache or removing it.

The tool uses only Python's standard library. Its parser and binary layout are
covered by \`tests/test_ps3_shader_cache.py\`.
