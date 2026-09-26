#!/usr/bin/env bash
# Rebuilds the zips that are stored in pieces (GitHub refuses files over 100 MB),
# then checks every zip against patches.md5.
set -euo pipefail
cd "$(dirname "$0")"

for first in *.zip.part00; do
    zip=${first%.part00}
    echo "joining $zip"
    cat "$zip".part[0-9][0-9] > "$zip"
done

md5sum -c patches.md5
