#!/usr/bin/env bash
# Generate the three title-specific caches from locally installed game data.
# Game PK3 files are read in place and are never copied into the output tree.
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "Usage: $0 <allied-assault-main> <spearhead-mainta> <breakthrough-maintt> <output-root>" >&2
    exit 2
fi

base_main="$(realpath "$1")"
spearhead_data="$(realpath "$2")"
breakthrough_data="$(realpath "$3")"
output_root="$(mkdir -p "$4" && realpath "$4")"

for directory in "$base_main" "$spearhead_data" "$breakthrough_data"; do
    [[ -d "$directory" ]] || {
        echo "error: game data directory not found: $directory" >&2
        exit 1
    }
done

script_dir="$(cd "$(dirname "$0")" && pwd)"
generator="$script_dir/ps3_shader_cache_from_pk3.py"

mkdir -p \
    "$output_root/main/scripts" \
    "$output_root/mainta/scripts" \
    "$output_root/maintt/scripts"

echo "==> Allied Assault shader cache"
python3 "$generator" \
    --main-dir "$base_main" \
    --output "$output_root/main/scripts/ps3_shader_cache.bin"

echo "==> Spearhead shader cache (base main + mainta overlay)"
python3 "$generator" \
    --main-dir "$base_main" \
    --expansion-dir "$spearhead_data" \
    --output "$output_root/mainta/scripts/ps3_shader_cache_mainta.bin"

echo "==> Breakthrough shader cache (base main + maintt overlay)"
python3 "$generator" \
    --main-dir "$base_main" \
    --expansion-dir "$breakthrough_data" \
    --output "$output_root/maintt/scripts/ps3_shader_cache_maintt.bin"

echo
echo "Generated title-specific caches under: $output_root"
