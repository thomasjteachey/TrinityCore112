#!/bin/bash
# Clone Kalimdor (map 1) server-side terrain data to the new Tanaris BG map id 1620.
#
# Server data files are named by map id via printf "%03u", so map 1 is "001..."
# and map 1620 is "1620..." (the width-3 spec does not truncate a 4-digit id).
# The map id is not embedded inside any of these files -- .map tiles, .vmtree /
# .vmtile and .mmap / .mmtile all key off the filename alone -- so a byte copy
# under the new name is a complete clone. This is the same trick the Obsidian
# Colosseum used for 615 -> 1615.
#
# Every destination name is new, so nothing is overwritten. Files are still
# written under a dot-prefixed temp name and renamed into place, so a
# half-written file is never visible under its final name to a running server.

set -u

SRC_ID=001
DST_ID=1620

clone_dir()
{
    local dir="$1" pattern="$2"
    local count=0 skipped=0

    shopt -s nullglob
    for src in "$dir"/${pattern}; do
        local base dst tmp
        base=$(basename "$src")
        dst="$dir/${DST_ID}${base#${SRC_ID}}"
        tmp="$dir/.clone_tmp_${DST_ID}${base#${SRC_ID}}"

        if [ -e "$dst" ]; then
            skipped=$((skipped + 1))
            continue
        fi

        cp "$src" "$tmp" || { echo "FAILED copying $src"; return 1; }
        mv "$tmp" "$dst" || { echo "FAILED renaming to $dst"; return 1; }
        count=$((count + 1))
    done
    shopt -u nullglob

    echo "    $dir/$pattern -> $count copied, $skipped already present"
}

clone_server()
{
    local root="$1"
    echo "=== $root ==="
    if [ ! -d "$root/maps" ]; then
        echo "    no maps dir, skipping"
        return 0
    fi

    clone_dir "$root/maps"  "${SRC_ID}*.map"     || return 1
    clone_dir "$root/vmaps" "${SRC_ID}.vmtree"   || return 1
    clone_dir "$root/vmaps" "${SRC_ID}_*.vmtile" || return 1
    clone_dir "$root/mmaps" "${SRC_ID}.mmap"     || return 1
    clone_dir "$root/mmaps" "${SRC_ID}*.mmtile"  || return 1

    echo "    totals for ${DST_ID}:"
    echo "      maps  $(ls "$root/maps"  | grep -c "^${DST_ID}")"
    echo "      vmaps $(ls "$root/vmaps" | grep -c "^${DST_ID}")"
    echo "      mmaps $(ls "$root/mmaps" | grep -c "^${DST_ID}")"
    du -shc "$root"/maps/${DST_ID}* "$root"/vmaps/${DST_ID}* "$root"/mmaps/${DST_ID}* 2>/dev/null | tail -1
}

clone_server /home/brokilodeluxe/wow/servers/tc-lplus-dev/data
clone_server /home/brokilodeluxe/wow/servers/tc-legionnaireplus/data

echo "DONE"
