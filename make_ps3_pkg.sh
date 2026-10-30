#!/usr/bin/env bash
# make_ps3_pkg.sh -- build an installable PS3 .pkg (CFW / HEN) for the OpenMoHAA
# PS3 port with the ps3dev / bucanero PSL1GHT toolchain.
#
# Pipeline (same as PSL1GHT's ppu_rules "%.pkg" rule):
#   ELF -> ppu-strip -> sprxlinker -> make_self_npdrm (or fself) -> USRDIR/EBOOT.BIN
#   sfo / sfo.py  -> PARAM.SFO (TITLE_ID = APPID)
#   pkg / pkg.py  -> <name>.pkg
#   package_finalize on a copy -> <name>.gnpdrm.pkg
#
# Put this file in the root of the source tree (next to CMakeLists.txt) and run
# it after a successful build:
#
#   ./make_ps3_pkg.sh                       # auto-detects the ELF in build-ps3/
#   ./make_ps3_pkg.sh --build-dir build-ps3-rsxtest --name OpenMoHAA-RSXTEST
#   ./make_ps3_pkg.sh --elf build-ps3/bin/openmohaa
#
# Needs: PS3DEV (and PSL1GHT) exported, as for the normal build.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR="build-ps3"
ELF=""
APPID=""
TITLE="OpenMoHAA"
ICON=""
PIC1=""
SND0=""
EXTRA_USRDIR=""
OUT_DIR="."
NAME=""
SFO_XML=""
SERVICE_ID="UP0001"
FINALIZE=1
SELF_TYPE="npdrm"
FORCE=0
KEEP_WORK=0

usage() {
    cat <<'EOF'
Usage: make_ps3_pkg.sh [options]

  --build-dir DIR     CMake build directory (default: build-ps3). Used to find
                      the ELF and to read OPENMOHAA_PS3_APPID from CMakeCache.txt.
  --elf FILE          The linked ELF. A file named *.stripped.elf is taken as
                      already stripped + sprxlinked (the ps3_self intermediate)
                      and those two steps are skipped.
  --appid ID          9-character TITLE_ID. Must equal the APPID compiled into
                      the binary (OPENMOHAA_PS3_APPID); the game's data and log
                      folder is /dev_hdd0/game/<ID>/USRDIR. Default: the value
                      in CMakeCache.txt, else OMOHAA020.
  --title TEXT        Name shown in the XMB (default: OpenMoHAA).
  --icon FILE         ICON0.PNG source; resized to 320x176 when ImageMagick or
                      Pillow is available (default: assets/xmb/ICON0.PNG).
  --pic1 FILE         optional PIC1.PNG (background, 1920x1080).
  --snd0 FILE         optional PS3 XMB hover sound (RIFF ATRAC3/ATRAC3+); copied as SND0.AT3,
                      SND01.AT3, and SND0_01.AT3 (PS3-compatible aliases).
  --extra-usrdir DIR  copy DIR's contents into the package's USRDIR (game data).
                      Not recommended: MOHAA data is large. Copy it over FTP or
                      USB instead.
  --sfo-xml FILE      SFO template (default: $PS3DEV/bin/sfo.xml).
  --service-id ID     content-id prefix (default: UP0001).
  --out-dir DIR       where the .pkg files go (default: current directory).
  --name NAME         output base name (default: OpenMoHAA-PS3_<APPID>).
  --self-type TYPE    npdrm (default): make_self_npdrm, signed with the content id.
                      fself: plain fake-signed self (fself / fself.py / make_self),
                      the kind the ps3_self CMake target produces. Use this if an
                      npdrm EBOOT.BIN exits straight back to the XMB.
  --no-finalize       skip the package_finalize (.gnpdrm.pkg) step.
  --force             continue when --appid differs from the build's APPID.
  --keep-work         keep the temporary work directory (for debugging).
  -h, --help          this text.

Environment: PS3DEV, PSL1GHT (as for the normal build). SFO_TOOL / PKG_TOOL set the
sfo / pkg tool names to try (default: the .py tools like ppu_rules, then the C ones;
a tool that fails is skipped). SFO_RUNNER / PKG_RUNNER prefix the command (e.g.
PKG_RUNNER=python2). CONTENT_ID overrides
the generated content id (36 chars, e.g. UP0001-OMOHAA020_00-0000000000000000).
EOF
}

die()  { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }
warn() { echo "warning: $*" >&2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)    BUILD_DIR="${2:?}"; shift 2 ;;
        --elf)          ELF="${2:?}"; shift 2 ;;
        --appid)        APPID="${2:?}"; shift 2 ;;
        --title)        TITLE="${2:?}"; shift 2 ;;
        --icon)         ICON="${2:?}"; shift 2 ;;
        --pic1)         PIC1="${2:?}"; shift 2 ;;
        --snd0)         SND0="${2:?}"; shift 2 ;;
        --extra-usrdir) EXTRA_USRDIR="${2:?}"; shift 2 ;;
        --sfo-xml)      SFO_XML="${2:?}"; shift 2 ;;
        --service-id)   SERVICE_ID="${2:?}"; shift 2 ;;
        --out-dir)      OUT_DIR="${2:?}"; shift 2 ;;
        --name)         NAME="${2:?}"; shift 2 ;;
        --self-type)    SELF_TYPE="${2:?}"; shift 2 ;;
        --no-finalize)  FINALIZE=0; shift ;;
        --force)        FORCE=1; shift ;;
        --keep-work)    KEEP_WORK=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              usage >&2; die "unknown option: $1" ;;
    esac
done

# ---- toolchain ---------------------------------------------------------------

if [ -z "${PS3DEV:-}" ] && [ -d /usr/local/ps3dev ]; then
    export PS3DEV=/usr/local/ps3dev
fi
[ -n "${PS3DEV:-}" ] || die "PS3DEV is not set (source your ps3dev environment first)"
export PSL1GHT="${PSL1GHT:-$PS3DEV}"

find_tool() {
    local name dir
    for name in "$@"; do
        if command -v "$name" >/dev/null 2>&1; then
            command -v "$name"
            return 0
        fi
        for dir in "$PS3DEV/bin" "$PS3DEV/ppu/bin" "$PSL1GHT/bin" "$PSL1GHT/host/bin"; do
            if [ -x "$dir/$name" ]; then
                echo "$dir/$name"
                return 0
            fi
        done
    done
    return 1
}

need_tool() {   # need_tool VAR description name...
    local var="$1" what="$2" path
    shift 2
    path="$(find_tool "$@")" || die "$what not found (looked for: $*) -- is PS3DEV=$PS3DEV correct?"
    printf -v "$var" '%s' "$path"
}

need_tool STRIP     "ppu-strip"            ppu-strip
need_tool SPRX      "sprxlinker"           sprxlinker
case "$SELF_TYPE" in
    npdrm) need_tool SELF_TOOL "make_self_npdrm" make_self_npdrm ;;
    fself) need_tool SELF_TOOL "fself (fself / fself.py / make_self)" fself fself.py make_self ;;
    *)     die "--self-type must be npdrm or fself (got '$SELF_TYPE')" ;;
esac
# PSL1GHT's own ppu_rules (bucanero fork) calls the Python tools (pkg.py / sfo.py),
# so those are tried first; the newer C versions (sfo / pkg) are the fallback. A
# tool that fails (e.g. pkg.py needing a Python 2 pkgcrypt module) is skipped and
# the next candidate is tried. SFO_TOOL / PKG_TOOL force a list of names (e.g.
# PKG_TOOL=pkg), PKG_RUNNER / SFO_RUNNER prefix the command (e.g. PKG_RUNNER=python2).
build_list() {   # build_list ARRAYNAME description name...
    local arr="$1" what="$2" n p found=0
    shift 2
    eval "$arr=()"
    for n in "$@"; do
        if p="$(find_tool "$n")"; then
            eval "$arr+=(\"\$p\")"
            found=1
        fi
    done
    [ "$found" -eq 1 ] || die "$what not found (looked for: $*) -- is PS3DEV=$PS3DEV correct?"
}
# shellcheck disable=SC2086
build_list SFO_LIST "sfo tool" ${SFO_TOOL:-sfo.py sfo}
# shellcheck disable=SC2086
build_list PKG_LIST "pkg tool" ${PKG_TOOL:-pkg.py pkg}
FINALIZER=""
if [ "$FINALIZE" -eq 1 ]; then
    FINALIZER="$(find_tool package_finalize || true)"
    if [ -z "$FINALIZER" ]; then
        warn "package_finalize not found: only the plain .pkg will be produced"
        FINALIZE=0
    fi
fi

if [ -z "$SFO_XML" ]; then
    SFO_XML="$PS3DEV/bin/sfo.xml"
fi
[ -f "$SFO_XML" ] || die "SFO template not found: $SFO_XML (use --sfo-xml)"

# ---- APPID / content id ------------------------------------------------------

cache_appid=""
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cache_appid="$(sed -n 's/^OPENMOHAA_PS3_APPID:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -n1)"
fi

if [ -n "$APPID" ] && [ -n "$cache_appid" ] && [ "$APPID" != "$cache_appid" ]; then
    msg="--appid $APPID differs from OPENMOHAA_PS3_APPID=$cache_appid compiled into $BUILD_DIR"
    if [ "$FORCE" -eq 1 ]; then
        warn "$msg (continuing: the game will look for its data in the WRONG folder)"
    else
        die "$msg. Rebuild with -DOPENMOHAA_PS3_APPID=$APPID or pass --force."
    fi
fi
if [ -z "$APPID" ]; then
    APPID="${cache_appid:-OMOHAA020}"
    if [ -z "$cache_appid" ]; then
        warn "no CMakeCache.txt APPID found, assuming $APPID"
    fi
fi
case "$APPID" in
    [A-Z0-9][A-Z0-9][A-Z0-9][A-Z0-9][A-Z0-9][A-Z0-9][A-Z0-9][A-Z0-9][A-Z0-9]) ;;
    *) die "APPID must be exactly 9 characters A-Z / 0-9 (got '$APPID')" ;;
esac

CONTENT_ID="${CONTENT_ID:-${SERVICE_ID}-${APPID}_00-0000000000000000}"
[ "${#CONTENT_ID}" -eq 36 ] || die "content id must be 36 characters (got ${#CONTENT_ID}: $CONTENT_ID)"

[ -n "$NAME" ] || NAME="OpenMoHAA-PS3_${APPID}"

# ---- locate the ELF ----------------------------------------------------------

if [ -z "$ELF" ]; then
    [ -d "$BUILD_DIR" ] || die "build directory not found: $BUILD_DIR (use --build-dir or --elf)"
    candidates=()
    while IFS= read -r f; do
        case "$f" in *.stripped.elf) continue ;; esac
        # linked programs only: "relocatable" (.o, partial links) is not one
        if file -b "$f" | grep -E 'ELF 64-bit MSB (executable|shared object).*PowerPC' >/dev/null; then
            candidates+=("$f")
        fi
    done < <(find "$BUILD_DIR" -type f -size +1M 2>/dev/null | sort)
    if [ "${#candidates[@]}" -eq 0 ]; then
        die "no PowerPC ELF found under $BUILD_DIR -- build first, or pass --elf"
    elif [ "${#candidates[@]}" -gt 1 ]; then
        printf 'candidates:\n' >&2
        printf '  %s\n' "${candidates[@]}" >&2
        die "more than one ELF found, choose with --elf"
    fi
    ELF="${candidates[0]}"
fi
[ -f "$ELF" ] || die "ELF not found: $ELF"
info "tools:      sfo=$(basename "${SFO_LIST[0]}") pkg=$(basename "${PKG_LIST[0]}") self=$(basename "$SELF_TOOL")"
info "ELF:        $ELF"
info "TITLE_ID:   $APPID   (installs to /dev_hdd0/game/$APPID)"
info "content id: $CONTENT_ID"

# ---- work tree ---------------------------------------------------------------

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ps3pkg.XXXXXX")"
cleanup() {
    if [ "$KEEP_WORK" -eq 1 ]; then
        info "work directory kept: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

PKGDIR="$WORK/pkg"
mkdir -p "$PKGDIR/USRDIR" "$OUT_DIR"

# 1. stripped + sprxlinked ELF
PREPARED="$WORK/app.stripped.elf"
case "$ELF" in
    *.stripped.elf)
        info "taking $ELF as already stripped and sprxlinked"
        cp "$ELF" "$PREPARED"
        ;;
    *)
        info "strip + sprxlinker"
        "$STRIP" "$ELF" -o "$PREPARED"
        "$SPRX" "$PREPARED"
        ;;
esac

# 2. NPDRM self -> USRDIR/EBOOT.BIN
info "self ($SELF_TYPE): $(basename "$SELF_TOOL")"
if [ "$SELF_TYPE" = "npdrm" ]; then
    "$SELF_TOOL" "$PREPARED" "$PKGDIR/USRDIR/EBOOT.BIN" "$CONTENT_ID" >/dev/null
else
    "$SELF_TOOL" "$PREPARED" "$PKGDIR/USRDIR/EBOOT.BIN" >/dev/null
fi
[ -s "$PKGDIR/USRDIR/EBOOT.BIN" ] || die "$(basename "$SELF_TOOL") produced no EBOOT.BIN"

# 3. PARAM.SFO
info "PARAM.SFO"
sfo_ok=0
for cand in "${SFO_LIST[@]}"; do
    rm -f "$PKGDIR/PARAM.SFO"
    # shellcheck disable=SC2086
    if ${SFO_RUNNER:-} "$cand" --title "$TITLE" --appid "$APPID" -f "$SFO_XML" "$PKGDIR/PARAM.SFO" >/dev/null \
        && [ -s "$PKGDIR/PARAM.SFO" ]; then
        sfo_ok=1
        info "  made with $(basename "$cand")"
        break
    fi
    warn "$(basename "$cand") failed, trying the next sfo tool"
done
[ "$sfo_ok" -eq 1 ] || die "no sfo tool produced a PARAM.SFO"

# 4. icon / background
if [ -z "$ICON" ]; then
    ICON="$REPO_DIR/assets/xmb/ICON0.PNG"
fi
fit_png() {   # fit_png SRC DST WxH
    local src="$1" dst="$2" size="$3" w h
    w="${size%x*}"; h="${size#*x}"
    if command -v magick >/dev/null 2>&1; then
        magick "$src" -resize "$size" -background none -gravity center -extent "$size" "PNG32:$dst"
    elif command -v convert >/dev/null 2>&1; then
        convert "$src" -resize "$size" -background none -gravity center -extent "$size" "PNG32:$dst"
    elif python3 -c 'import PIL' >/dev/null 2>&1; then
        python3 - "$src" "$dst" "$w" "$h" <<'PY'
import sys
from PIL import Image
src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
im = Image.open(src).convert("RGBA")
im.thumbnail((w, h), Image.LANCZOS)
canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
canvas.alpha_composite(im, ((w - im.width) // 2, (h - im.height) // 2))
canvas.save(dst, "PNG")
PY
    else
        warn "no ImageMagick/Pillow: copying $src unscaled (the XMB wants $size)"
        cp "$src" "$dst"
    fi
}
if [ -f "$ICON" ]; then
    fit_png "$ICON" "$PKGDIR/ICON0.PNG" 320x176
else
    warn "icon not found ($ICON): package will have no ICON0.PNG"
fi
if [ -n "$PIC1" ]; then
    [ -f "$PIC1" ] || die "PIC1 not found: $PIC1"
    fit_png "$PIC1" "$PKGDIR/PIC1.PNG" 1920x1080
fi
if [ -n "$SND0" ]; then
    [ -f "$SND0" ] || die "SND0 not found: $SND0"
    python3 - "$SND0" <<'PY'
import pathlib, struct, sys
p = pathlib.Path(sys.argv[1])
b = p.read_bytes()
if len(b) < 12 or b[:4] != b"RIFF" or b[8:12] != b"WAVE":
    raise SystemExit(f"error: {p} is not a RIFF/WAVE SND0.AT3 file")
pos, fmt_ok, data_ok = 12, False, False
while pos + 8 <= len(b):
    tag = b[pos:pos+4]
    size = struct.unpack_from("<I", b, pos + 4)[0]
    start = pos + 8
    if start + size > len(b):
        raise SystemExit(f"error: malformed RIFF chunk in {p}")
    if tag == b"fmt " and size >= 16:
        codec = struct.unpack_from("<H", b, start)[0]
        if codec not in (0x0270, 0xFFFE):
            raise SystemExit(f"error: {p} has unsupported audio format tag 0x{codec:04x}")
        fmt_ok = True
    elif tag == b"data" and size:
        data_ok = True
    pos = start + size + (size & 1)
if not (fmt_ok and data_ok):
    raise SystemExit(f"error: {p} is missing a supported ATRAC fmt/data chunk")
PY
    cp "$SND0" "$PKGDIR/SND0.AT3"
    cp "$SND0" "$PKGDIR/SND01.AT3"
    cp "$SND0" "$PKGDIR/SND0_01.AT3"
    info "copied validated XMB sound to SND0.AT3, SND01.AT3, and SND0_01.AT3"
fi

# 5. optional payload
if [ -n "$EXTRA_USRDIR" ]; then
    [ -d "$EXTRA_USRDIR" ] || die "--extra-usrdir is not a directory: $EXTRA_USRDIR"
    info "copying $EXTRA_USRDIR into USRDIR (this can be big)"
    cp -a "$EXTRA_USRDIR"/. "$PKGDIR/USRDIR/"
fi

# 6. pack
OUT_PKG="$OUT_DIR/$NAME.pkg"
info "pkg -> $OUT_PKG"
pkg_ok=0
for cand in "${PKG_LIST[@]}"; do
    rm -f "$OUT_PKG"
    # shellcheck disable=SC2086
    if ${PKG_RUNNER:-} "$cand" --contentid "$CONTENT_ID" "$PKGDIR/" "$OUT_PKG" >/dev/null \
        && [ -s "$OUT_PKG" ]; then
        pkg_ok=1
        info "  made with $(basename "$cand")"
        break
    fi
    warn "$(basename "$cand") failed, trying the next pkg tool"
done
[ "$pkg_ok" -eq 1 ] || die "no pkg tool produced a package"

# 7. finalize (retail-signed copy for HEN / any CFW)
OUT_FINAL=""
if [ "$FINALIZE" -eq 1 ]; then
    OUT_FINAL="$OUT_DIR/$NAME.gnpdrm.pkg"
    info "package_finalize -> $OUT_FINAL"
    cp "$OUT_PKG" "$OUT_FINAL"
    "$FINALIZER" "$OUT_FINAL" >/dev/null
fi

echo
echo "Done."
ls -l "$OUT_PKG" ${OUT_FINAL:+"$OUT_FINAL"}
echo
echo "Install: copy a .pkg to a USB stick (or FTP it to /dev_hdd0/packages) and use"
echo "XMB > Game > Install Package Files. Try the .gnpdrm.pkg first on HEN; if one"
echo "variant is refused, try the other. Game data goes in"
echo "/dev_hdd0/game/$APPID/USRDIR (the folder the binary was compiled to use)."
