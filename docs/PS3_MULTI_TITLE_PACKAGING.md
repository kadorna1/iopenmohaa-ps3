# PS3 multi-title packages

The PS3 workflow builds three independent EBOOT/PKG variants from the same source
and toolchain:

| Product | PS3 TITLE_ID | Default target game |
|---|---|---:|
| Allied Assault | `OMOHAA020` | `0` (`TG_MOH`) |
| Breakthrough | `OMOHAB020` | `2` (`TG_MOHTT`) |
| Spearhead | `OMOHAS020` | `1` (`TG_MOHTA`) |

PS3 package TITLE_ID values are nine characters. The abbreviated forms
`OMOHA020`, `OMOHAB20`, and `OMOHAS20` are eight characters; this workflow
keeps the existing nine-character Allied Assault ID and uses nine-character
IDs for the expansion packages.

## Expansion data in package-local USRDIRs

The workflow copies the common runtime/config payload from
`assets/ps3_usrdir/` into every package, then stages only the matching
expansion directory:

| Product | Source in the repository | Destination after installation |
|---|---|---|
| Breakthrough | `assets/ps3_usrdir/maintt/` | `/dev_hdd0/game/OMOHAB020/USRDIR/maintt/` |
| Spearhead | `assets/ps3_usrdir/mainta/` | `/dev_hdd0/game/OMOHAS020/USRDIR/mainta/` |

Do not place the other expansion's directory in that package. The staging
helper explicitly filters `mainta` and `maintt` out of the common copy, then
copies only the one associated with the selected TITLE_ID. Allied Assault
does not receive either expansion directory.

The game-data payload itself is not currently tracked in this repository. Add
the game files you are entitled to use under the corresponding source directory
above; the workflow will include them on the next build. When a source directory
is absent or empty, Actions emits a warning rather than pretending that the
expansion data was packaged.

## Shared Allied Assault base-game lookup

The expansion executables keep their writable/configuration roots private to
their own package TITLE_ID, but the engine's default install/base path remains:

```
/dev_hdd0/game/OMOHAA020/USRDIR
```

The PS3 filesystem mounts both the package-local home paths and this shared
base path. That lets Breakthrough read its expansion content from its own
`USRDIR/maintt` and Spearhead from its own `USRDIR/mainta`, while common
Allied Assault assets remain available from:

```
/dev_hdd0/game/OMOHAA020/USRDIR/main
```

No copy of the base game's archives is added to the expansion PKGs. The base
game must already be installed under the Allied Assault TITLE_ID. The engine
searches both the package-local roots and the shared base root when building
its game search paths.

## Per-title writable state

Each EBOOT uses its own package-specific home/config/data/state directory:

- Allied Assault: `/dev_hdd0/game/OMOHAA020/USRDIR`
- Breakthrough: `/dev_hdd0/game/OMOHAB020/USRDIR`
- Spearhead: `/dev_hdd0/game/OMOHAS020/USRDIR`

Configuration, savegames, screenshots, state files and logs are stored under
that private path. The executable/binary path also remains package-local. Only
the engine's default install/base path is redirected to the shared Allied
Assault root.

## Build and incremental caches

GitHub Actions configures three separate CMake build trees, one per package
TITLE_ID and `com_target_game` default. This prevents an EBOOT compiled for one
product from being paired with another product's PARAM.SFO. The compiler cache,
host dependency image and extracted PS3 toolchain remain shared.

Each build tree persists separately and stores its source-hash manifest after a
successful build/package pass. Subsequent runs compare all three manifests,
normalize tracked source timestamps once, and re-stamp the union of changed
files. This preserves incremental Ninja builds across the three variants.
