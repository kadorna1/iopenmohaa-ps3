#!/usr/bin/env python3
"""Build a title-specific PS3 shader cache from a game data directory.

Point --main-dir at the directory containing that title's .pk3 archives:
Allied Assault main/, Spearhead mainta/, or Breakthrough maintt/. Only direct
scripts/*.shader entries are indexed. Existing loose scripts are included
with normal loose-file search-path priority.
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


def build_cache_from_pk3(main_dir: Path) -> tuple[bytes, dict[str, int]]:
    """Index unique shader files with the same PK3 precedence as OpenMoHAA.

    OpenMoHAA sorts archive names ascending then prepends each archive to its
    search path, so the lexically last archive has highest priority. Loose
    main/scripts files are added above every archive. The renderer then
    uniques filenames before reading them, so retain the winning copy only.
    """
    if not main_dir.is_dir():
        raise ShaderCacheError(f"main directory not found: {main_dir}")

    archives = sorted(
        (path for path in main_dir.iterdir()
         if path.is_file() and path.suffix.casefold() == ".pk3"),
        key=lambda path: path.name.casefold(),
    )
    if not archives:
        raise ShaderCacheError(f"no .pk3 archives found directly in {main_dir}")

    # key -> (filename as exposed by the winning search path, source bytes)
    selected: dict[str, tuple[str, bytes]] = {}
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

    loose_dir = main_dir / "scripts"
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
            selected[key] = (path.name, path.read_bytes())
            loose_count += 1

    if not selected:
        raise ShaderCacheError(
            f"no direct scripts/*.shader files found in {len(archives)} PK3 archives"
        )

    with tempfile.TemporaryDirectory(prefix="omh-ps3-shaders-") as temp:
        scripts_dir = Path(temp)
        for filename, content in selected.values():
            # Names came from a single path component in the PK3 entry.
            (scripts_dir / filename).write_bytes(content)
        cache, stats = build_cache(scripts_dir)

    stats.update({
        "pk3_archives": len(archives),
        "overridden_shader_files": overridden,
        "loose_shader_files": loose_count,
    })
    return cache, stats


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build the OpenMoHAA PS3 shader cache from extracted game PK3 archives."
    )
    parser.add_argument(
        "--main-dir", required=True, type=Path,
        help="title data directory (main, mainta, or maintt) containing *.pk3 archives",
    )
    parser.add_argument(
        "--output", required=True, type=Path,
        help="output file, normally a local ps3_shader_cache.bin",
    )
    args = parser.parse_args(argv)

    try:
        cache, stats = build_cache_from_pk3(args.main_dir.expanduser().resolve())
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
        f"scanned {stats['pk3_archives']} PK3 archives; "
        f"{stats['overridden_shader_files']} filename overrides; "
        f"{stats['loose_shader_files']} loose files; "
        f"{stats['cache_bytes']} bytes -> {args.output}"
    )
    print("The cache contains game-derived shader text; keep it with your own game data.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
