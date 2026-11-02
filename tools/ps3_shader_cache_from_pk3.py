#!/usr/bin/env python3
"""Build a title-specific PS3 shader cache from the installed game PK3s.

For an expansion, pass the Allied Assault main/ directory as --main-dir and
the expansion's mainta/ or maintt/ directory as --expansion-dir. Expansion
shader filenames override base-game files, matching the mounted search-path
priority used by OpenMoHAA.
"""
from __future__ import annotations

import argparse
import os
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Iterable

from ps3_shader_cache import ShaderCacheError, build_cache


def _shader_entries(archive: zipfile.ZipFile):
    for info in archive.infolist():
        if info.is_dir():
            continue
        name = info.filename.replace("\\", "/")
        parts = name.split("/")
        if (
            len(parts) == 2
            and parts[0].casefold() == "scripts"
            and parts[1].casefold().endswith(".shader")
            and parts[1]
        ):
            yield info, parts[1]


def _merge_data_directory(
    data_dir: Path,
    selected: dict[str, tuple[str, bytes]],
) -> tuple[int, int, int]:
    """Overlay one data directory (higher-priority later directories win).

    Within a data directory, OpenMoHAA puts later-sorted PK3s above earlier
    PK3s, then loose files above all PK3 entries. Call this for base data first
    and expansion data second to mirror the expansion search path.
    """
    if not data_dir.is_dir():
        raise ShaderCacheError(f"game data directory not found: {data_dir}")

    archives = sorted(
        (path for path in data_dir.iterdir()
         if path.is_file() and path.suffix.casefold() == ".pk3"),
        key=lambda path: path.name.casefold(),
    )
    if not archives:
        raise ShaderCacheError(f"no .pk3 archives found directly in {data_dir}")

    overridden = 0
    for archive_path in archives:
        try:
            with zipfile.ZipFile(archive_path, "r") as archive:
                for info, filename in _shader_entries(archive):
                    key = filename.casefold()
                    if key in selected:
                        overridden += 1
                    selected[key] = (filename, archive.read(info))
        except (OSError, zipfile.BadZipFile, zipfile.LargeZipFile) as exc:
            raise ShaderCacheError(f"cannot read PK3 archive {archive_path}: {exc}") from exc

    loose_dir = data_dir / "scripts"
    loose_count = 0
    if loose_dir.is_dir():
        for path in sorted(
            (item for item in loose_dir.iterdir()
             if item.is_file() and item.suffix.casefold() == ".shader"),
            key=lambda item: item.name.casefold(),
        ):
            key = path.name.casefold()
            if key in selected:
                overridden += 1
            try:
                selected[key] = (path.name, path.read_bytes())
            except OSError as exc:
                raise ShaderCacheError(f"cannot read loose shader file {path}: {exc}") from exc
            loose_count += 1

    return len(archives), overridden, loose_count


def build_cache_from_pk3(
    main_dir: Path,
    expansion_dir: Path | None = None,
) -> tuple[bytes, dict[str, int]]:
    """Index the base files plus optional expansion overlay in FS priority."""
    selected: dict[str, tuple[str, bytes]] = {}
    archive_count = 0
    overridden = 0
    loose_count = 0
    directories = [main_dir]
    if expansion_dir is not None:
        directories.append(expansion_dir)

    for data_dir in directories:
        archives, duplicate_count, loose = _merge_data_directory(data_dir, selected)
        archive_count += archives
        overridden += duplicate_count
        loose_count += loose

    if not selected:
        raise ShaderCacheError("no direct scripts/*.shader files found in the supplied PK3 archives")

    with tempfile.TemporaryDirectory(prefix="omh-ps3-shaders-") as temp:
        scripts_dir = Path(temp)
        for filename, content in selected.values():
            # Names came from a single path component in the PK3 entry.
            (scripts_dir / filename).write_bytes(content)
        cache, stats = build_cache(scripts_dir)

    stats.update({
        "pk3_archives": archive_count,
        "overridden_shader_files": overridden,
        "loose_shader_files": loose_count,
        "data_directories": len(directories),
    })
    return cache, stats


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build a PS3 shader cache from base-game and optional expansion PK3 archives."
    )
    parser.add_argument(
        "--main-dir", required=True, type=Path,
        help="Allied Assault main/ directory containing base-game *.pk3 archives",
    )
    parser.add_argument(
        "--expansion-dir", type=Path,
        help="optional Spearhead mainta/ or Breakthrough maintt/ overlay directory",
    )
    parser.add_argument(
        "--output", required=True, type=Path,
        help="output file, e.g. ps3_shader_cache.bin or a title-specific expansion cache",
    )
    args = parser.parse_args(argv)

    try:
        cache, stats = build_cache_from_pk3(
            args.main_dir.expanduser().resolve(),
            args.expansion_dir.expanduser().resolve() if args.expansion_dir else None,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            dir=args.output.parent,
            prefix=args.output.name + ".",
            suffix=".tmp",
            delete=False,
        ) as temp:
            temp.write(cache)
            temp_path = Path(temp.name)
        os.replace(temp_path, args.output)
    except (OSError, ShaderCacheError) as exc:
        print(f"ps3_shader_cache_from_pk3: error: {exc}", file=sys.stderr)
        return 1

    print(
        "PS3 shader cache generated: "
        f"{stats['definitions']} definitions from {stats['source_files']} unique shader files; "
        f"scanned {stats['pk3_archives']} PK3 archives across {stats['data_directories']} data directories; "
        f"{stats['overridden_shader_files']} filename overrides; "
        f"{stats['loose_shader_files']} loose files; "
        f"{stats['cache_bytes']} bytes -> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
