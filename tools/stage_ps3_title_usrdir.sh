#!/usr/bin/env bash
# Stage the common runtime payload plus only the selected title's expansion
# directory into a package-local USRDIR.
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $0 <TITLE_ID> <source-usrdir> <destination-usrdir>" >&2
    exit 2
fi

appid="$1"
source_usrdir="$2"
dest_usrdir="$3"

[[ -d "$source_usrdir" ]] || {
    echo "error: source USRDIR does not exist: $source_usrdir" >&2
    exit 1
}

case "$appid" in
    OMOHAA020) expansion_dir="" ;;
    OMOHAB020) expansion_dir="maintt" ;;
    OMOHAS020) expansion_dir="mainta" ;;
    *)
        echo "error: unsupported PS3 title ID: $appid" >&2
        exit 1
        ;;
esac

mkdir -p "$dest_usrdir"

# Copy common runtime/config files, but keep expansion data isolated.
shopt -s dotglob nullglob
for item in "$source_usrdir"/*; do
    [[ -e "$item" ]] || continue
    name="${item##*/}"
    case "$name" in
        mainta|maintt) continue ;;
    esac
    cp -a -- "$item" "$dest_usrdir/"
done

if [[ -n "$expansion_dir" ]]; then
    mkdir -p "$dest_usrdir/$expansion_dir"
    source_expansion="$source_usrdir/$expansion_dir"
    if [[ -d "$source_expansion" ]]; then
        cp -a -- "$source_expansion/." "$dest_usrdir/$expansion_dir/"
        if ! find "$source_expansion" -type f -print -quit | grep -q .; then
            echo "::warning::No expansion data files found in $source_expansion; $appid will have an empty $expansion_dir directory."
        fi
    else
        echo "::warning::No expansion payload directory at $source_expansion; add the expansion's files there before publishing a playable package."
    fi
fi

echo "Staged $appid USRDIR:"
find "$dest_usrdir" -mindepth 1 -maxdepth 3 -print | sort
