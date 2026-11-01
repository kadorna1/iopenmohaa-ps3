# PS3 multi-title packages

The PS3 workflow builds three independent EBOOT/PKG variants from the same source
and toolchain:

| Product | PS3 TITLE_ID | Default target game |
|---|---|---:|
| Allied Assault | `OMOHAA020` | `0` (`TG_MOH`) |
| Breakthrough | `OMOHAB020` | `1` (`TG_MOHTA`) |
| Spearhead | `OMOHAS020` | `2` (`TG_MOHTT`) |

PS3 package TITLE_ID values are nine characters. The abbreviated forms
`OMOHA020`, `OMOHAB20`, and `OMOHAS20` are eight characters; this workflow
keeps the existing nine-character Allied Assault ID and uses nine-character
IDs for the expansion packages.

## Shared read-only game data

All three builds set the engine's default install/base path to:

```
/dev_hdd0/game/OMOHAA020/USRDIR
```

The Allied Assault files are read from:

```
/dev_hdd0/game/OMOHAA020/USRDIR/main
```

Expansion selection uses OpenMoHAA's native directories under that same root:
Breakthrough uses `mainta`, and Spearhead uses `maintt`. The PKGs do not copy
or duplicate the user's game archives. The small `assets/ps3_usrdir`
startup/config payload is installed into each package's private USRDIR.

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
