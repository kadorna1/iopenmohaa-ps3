#!/usr/bin/env python3
"""Build an optional pre-indexed OpenMoHAA PS3 material-shader cache.

The input directory must contain the extracted scripts/*.shader files. No game
assets are bundled with this tool or committed to the repository.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import tempfile
from pathlib import Path
from typing import Iterable

MAGIC = b"OMHPSHC1"
VERSION = 1
MAX_SHADER_FILES = 4096
MAX_SHADER_ENTRIES = 262144
MAX_TEXT_BYTES = 128 * 1024 * 1024
SOURCE_RECORD_SIZE = 64
ENTRY_STRUCT = struct.Struct("<HHII64s")
HEADER_STRUCT = struct.Struct("<8sIIIIII")


class ShaderCacheError(ValueError):
    """Raised when the source scripts cannot be indexed safely."""


def _skip_trivia(data: bytes, pos: int, source: str) -> int:
    size = len(data)
    while pos < size:
        if data[pos] in b" \t\r\n\v\f":
            pos += 1
            continue
        if data.startswith(b"//", pos):
            end = data.find(b"\n", pos + 2)
            pos = size if end < 0 else end + 1
            continue
        if data.startswith(b"/*", pos):
            end = data.find(b"*/", pos + 2)
            if end < 0:
                raise ShaderCacheError(f"{source}: unterminated block comment")
            pos = end + 2
            continue
        break
    return pos


def _read_token(data: bytes, pos: int, source: str) -> tuple[str, int]:
    pos = _skip_trivia(data, pos, source)
    if pos >= len(data):
        return "", pos

    char = data[pos]
    if char in (ord("{"), ord("}")):
        return chr(char), pos + 1

    if char == ord('"'):
        start = pos + 1
        pos += 1
        escaped = False
        while pos < len(data):
            current = data[pos]
            if escaped:
                escaped = False
            elif current == ord("\\"):
                escaped = True
            elif current == ord('"'):
                try:
                    return data[start:pos].decode("utf-8"), pos + 1
                except UnicodeDecodeError as exc:
                    raise ShaderCacheError(f"{source}: non-UTF-8 quoted token") from exc
            pos += 1
        raise ShaderCacheError(f"{source}: unterminated quoted token")

    start = pos
    while pos < len(data):
        if data[pos] in b" \t\r\n\v\f{}":
            break
        if data.startswith(b"//", pos) or data.startswith(b"/*", pos):
            break
        pos += 1

    if pos == start:
        raise ShaderCacheError(f"{source}: unexpected byte at offset {pos}")
    try:
        return data[start:pos].decode("utf-8"), pos
    except UnicodeDecodeError as exc:
        raise ShaderCacheError(f"{source}: non-UTF-8 shader name") from exc


def _find_block_end(data: bytes, start: int, source: str) -> int:
    if start >= len(data) or data[start] != ord("{"):
        raise ShaderCacheError(f"{source}: expected '{{' at offset {start}")

    depth = 0
    pos = start
    while pos < len(data):
        if data.startswith(b"//", pos):
            end = data.find(b"\n", pos + 2)
            pos = len(data) if end < 0 else end + 1
            continue
        if data.startswith(b"/*", pos):
            end = data.find(b"*/", pos + 2)
            if end < 0:
                raise ShaderCacheError(f"{source}: unterminated block comment")
            pos = end + 2
            continue
        char = data[pos]
        if char == ord('"'):
            pos += 1
            escaped = False
            while pos < len(data):
                current = data[pos]
                if escaped:
                    escaped = False
                elif current == ord("\\"):
                    escaped = True
                elif current == ord('"'):
                    pos += 1
                    break
                pos += 1
            else:
                raise ShaderCacheError(f"{source}: unterminated quoted string")
            continue
        if char == ord("{"):
            depth += 1
        elif char == ord("}"):
            depth -= 1
            if depth == 0:
                return pos + 1
            if depth < 0:
                raise ShaderCacheError(f"{source}: unmatched closing brace")
        pos += 1

    raise ShaderCacheError(f"{source}: unterminated shader block")


def parse_shader_definitions(data: bytes, source: str = "<memory>") -> list[tuple[str, bytes]]:
    """Return (material_name, original_text_from_after_name_through_brace) pairs."""
    definitions: list[tuple[str, bytes]] = []
    pos = 0
    while True:
        pos = _skip_trivia(data, pos, source)
        if pos >= len(data):
            break
        if data[pos] == ord("}"):
            pos += 1
            continue
        if data[pos] == ord("{"):
            pos = _find_block_end(data, pos, source)
            continue

        name, after_name = _read_token(data, pos, source)
        if not name:
            break
        if name in ("{", "}"):
            pos = after_name
            continue

        block_start = _skip_trivia(data, after_name, source)
        if block_start >= len(data) or data[block_start] != ord("{"):
            raise ShaderCacheError(
                f"{source}: expected a material block after shader name {name!r}"
            )
        block_end = _find_block_end(data, block_start, source)
        # ParseShader expects its input pointer immediately after the name;
        # preserve comments/whitespace between the name and opening brace.
        definitions.append((name, data[after_name:block_end] + b"\0"))
        pos = block_end

    return definitions


def _source_files(scripts_dir: Path) -> list[Path]:
    if not scripts_dir.is_dir():
        raise ShaderCacheError(f"scripts directory not found: {scripts_dir}")
    files = sorted(
        (path for path in scripts_dir.iterdir()
         if path.is_file() and path.suffix.lower() == ".shader"),
        key=lambda path: path.name.casefold(),
    )
    if not files:
        raise ShaderCacheError(f"no *.shader files found in {scripts_dir}")
    if len(files) > MAX_SHADER_FILES:
        raise ShaderCacheError(
            f"{len(files)} shader files exceed renderer limit {MAX_SHADER_FILES}"
        )
    for path in files:
        encoded = path.name.encode("utf-8")
        if len(encoded) >= SOURCE_RECORD_SIZE:
            raise ShaderCacheError(
                f"shader filename exceeds {SOURCE_RECORD_SIZE - 1} UTF-8 bytes: {path.name}"
            )
    return files


def build_cache(scripts_dir: Path) -> tuple[bytes, dict[str, int]]:
    files = _source_files(scripts_dir)
    text_blob = bytearray()
    records: list[bytes] = []

    # The runtime index is linked by prepending records to each hash bucket.
    # Reversing the sorted file list preserves the legacy "earlier sorted file
    # wins" behavior when duplicate material names are present.
    for path in reversed(files):
        try:
            source_bytes = path.read_bytes()
        except OSError as exc:
            raise ShaderCacheError(f"cannot read {path}: {exc}") from exc
        for name, shader_text in parse_shader_definitions(source_bytes, path.name):
            encoded_name = name.encode("utf-8")
            if not encoded_name or len(encoded_name) >= 64:
                raise ShaderCacheError(
                    f"{path.name}: shader name must fit in 63 UTF-8 bytes: {name!r}"
                )
            if len(records) >= MAX_SHADER_ENTRIES:
                raise ShaderCacheError(
                    f"shader count exceeds renderer limit {MAX_SHADER_ENTRIES}"
                )
            text_offset = len(text_blob)
            text_blob.extend(shader_text)
            records.append(
                ENTRY_STRUCT.pack(
                    len(encoded_name), 0, text_offset, len(shader_text),
                    encoded_name.ljust(64, b"\0"),
                )
            )

    if not records:
        raise ShaderCacheError("no material shader definitions were found")
    if len(text_blob) > MAX_TEXT_BYTES:
        raise ShaderCacheError(
            f"shader text payload exceeds {MAX_TEXT_BYTES} bytes"
        )

    manifest = b"".join(
        path.name.encode("utf-8").ljust(SOURCE_RECORD_SIZE, b"\0")
        for path in files
    )
    header = HEADER_STRUCT.pack(
        MAGIC, VERSION, len(records), len(files), len(text_blob),
        SOURCE_RECORD_SIZE, ENTRY_STRUCT.size,
    )
    cache = header + manifest + b"".join(records) + bytes(text_blob)
    return cache, {
        "source_files": len(files),
        "definitions": len(records),
        "text_bytes": len(text_blob),
        "cache_bytes": len(cache),
    }


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Pre-index extracted OpenMoHAA material shader scripts for PS3."
    )
    parser.add_argument(
        "--scripts-dir", required=True, type=Path,
        help="directory containing the extracted scripts/*.shader files",
    )
    parser.add_argument(
        "--output", required=True, type=Path,
        help="output path (normally scripts/ps3_shader_cache.bin)",
    )
    args = parser.parse_args(argv)

    try:
        cache, stats = build_cache(args.scripts_dir)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            dir=args.output.parent, prefix=args.output.name + ".",
            suffix=".tmp", delete=False,
        ) as temp:
            temp.write(cache)
            temp_path = Path(temp.name)
        os.replace(temp_path, args.output)
    except (OSError, ShaderCacheError) as exc:
        print(f"ps3_shader_cache: error: {exc}", file=sys.stderr)
        return 1

    print(
        "PS3 material shader cache written: "
        f"{stats['definitions']} definitions, "
        f"{stats['source_files']} files, "
        f"{stats['text_bytes']} text bytes, "
        f"{stats['cache_bytes']} total bytes -> {args.output}"
    )
    print("Regenerate this file whenever scripts/*.shader content changes.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
