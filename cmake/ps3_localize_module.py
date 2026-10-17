#!/usr/bin/env python3
"""Localize all globally-defined symbols in a PS3 relocatable module except its ABI entrypoint.

This mirrors the proven ioQuake3/ioRTCW linked-module model: a game/cgame is
partial-linked into one object, then its private implementation symbols are
made local so its copies of q_shared/script helpers cannot clash with engine
or sibling module symbols. Undefined symbols remain undefined and resolve
against the final EBOOT.
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--keep", action="append", default=[])
    ap.add_argument("--readelf", default="", help="readelf to list COMDAT groups (default: derived from --objcopy)")
    ap.add_argument(
        "--weak-prefix", default="",
        help="rename every defined weak/COMDAT symbol (templates, inline "
             "functions, vtables, RTTI, guard variables) to PREFIX+name, so the "
             "module keeps private copies instead of the final link merging one "
             "copy shared by the engine, game and cgame",
    )
    args = ap.parse_args()

    keep = set(args.keep)
    proc = subprocess.run(
        [args.nm, "-g", "--defined-only", args.input],
        check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )

    names = []
    weak_names = []
    seen = set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[-1]
        sym_type = parts[-2] if len(parts) >= 3 else ""
        # GNU nm may print section/version noise; only use ordinary symbols.
        if not re.match(r"^[A-Za-z_.$][A-Za-z0-9_.$@]*$", name):
            continue
        if name in keep or name in seen:
            continue
        # Keep linker-set boundaries, absolute/toolchain metadata, and weak
        # symbols global. Weak/COMDAT C++ symbols include RTTI, vtables,
        # inline virtual functions, thunks and template instantiations.
        # Localizing them changes COMDAT binding and can leave relocations
        # pointing at a discarded section during the final EBOOT link.
        if name.startswith("__start_") or name.startswith("__stop_"):
            continue
        if sym_type.upper() == "A" or name in {"_GLOBAL_OFFSET_TABLE_", "__GLOBAL_OFFSET_TABLE_", ".TOC."}:
            continue
        if sym_type in {"W", "V", "w", "v", "u", "U"}:
            # Defined weak symbols are not localized (see above), but with a
            # prefix they can still be made private to this module: the engine,
            # game and cgame compile the same templates and inline functions
            # with different GAME_DLL/CGAME_DLL macros (allocators, printf,
            # error handlers). Left under one name the final link keeps ONE
            # copy for all three, and the engine ends up calling through a
            # module's never-initialised gi/cgi table. Renaming keeps each
            # module's references bound to its own copy, as with separate DLLs.
            if args.weak_prefix and sym_type in {"W", "V", "u"} and "@" not in name:
                seen.add(name)
                weak_names.append(name)
            continue
        seen.add(name)
        names.append(name)

    # COMDAT group signatures. The linker merges groups across objects by the
    # NAME of the signature symbol, and those symbols are usually local (not
    # visible to `nm -g`). If only the defined weak symbols were renamed, the
    # engine's group and this module's group would still share a signature, one
    # would be discarded, and the renamed symbols would end up "defined in
    # discarded section .group". So the signatures get the prefix too.
    if args.weak_prefix:
        readelf = args.readelf or re.sub(r"objcopy$", "readelf", args.objcopy)
        grp = subprocess.run(
            [readelf, "-gW", args.input],
            check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        for sig in re.findall(r"group section \[\s*\d+\]\s+`[^']*'\s+\[([^\]]+)\]", grp.stdout):
            if sig in keep or sig in seen or "@" in sig:
                continue
            seen.add(sig)
            weak_names.append(sig)

    out = Path(args.output)
    symfile = out.with_suffix(out.suffix + ".localize")
    symfile.write_text("\n".join(names) + ("\n" if names else ""))
    renamefile = out.with_suffix(out.suffix + ".weakrename")
    renamefile.write_text(
        "".join(f"{n} {args.weak_prefix}{n}\n" for n in weak_names)
    )

    cmd = [args.objcopy]
    if names:
        cmd += ["--localize-symbols", str(symfile)]
    if weak_names:
        cmd += ["--redefine-syms", str(renamefile)]
    if names or weak_names:
        subprocess.run(cmd + [args.input, args.output], check=True)
    else:
        # Nothing needs changing. Preserve the relocatable object exactly
        # rather than asking objcopy to consume empty symbol lists.
        shutil.copy2(args.input, args.output)
    print(
        f"ps3_localize_module.py: {len(names)} symbols localized, "
        f"{len(weak_names)} weak symbols renamed with prefix '{args.weak_prefix}'"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(f"ps3_localize_module.py: command failed with exit code {exc.returncode}", file=sys.stderr)
        raise
