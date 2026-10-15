#!/bin/bash
# The Gamesman's Hall arena (map 1532): install the server side into one realm.
#   install_chess.sh <tc-centurion|tc-centuriondev> check|apply
# DBCs: each live file must still be the one downloaded (stage/before.md5) and the staged one the
# patched one (stage/after.md5); backups as <file>.bak-chess-20261015.
# Map data: map 532's maps/vmaps/mmaps renamed to 1532 (work/), with the hall's tile
# 15325235.mmtile carved (chess_nav.py). New files only - refuses to overwrite anything.
# A running worldserver reads DBCs only at startup and never loads map 1532 before its Map.dbc has
# the row, so none of this touches a running realm.
set -euo pipefail
REALM="$1"; MODE="$2"
BASE=/home/brokilodeluxe/chess-arena-20261015
DATA=/home/brokilodeluxe/wow/servers/$REALM/data
TS=20261015
TABLES="Map BattlemasterList PvpDifficulty WorldSafeLocs WorldStateUI MapDifficulty"
CARVED_MD5=1adb8e13283d
[ -d "$DATA/dbc" ] || { echo "no $DATA/dbc"; exit 1; }

fail=0
for t in $TABLES; do
  live="$DATA/dbc/$t.dbc"; staged="$BASE/stage/dbc/$REALM/$t.dbc"
  want_before=$(awk -v f="$REALM/$t.dbc" '$2==f {print $1}' "$BASE/stage/before.md5")
  want_after=$(awk -v f="$REALM/$t.dbc" '$2==f {print $1}' "$BASE/stage/after.md5")
  cur=$(md5sum < "$live" | cut -c1-32); new=$(md5sum < "$staged" | cut -c1-32)
  if [ "$cur" = "$want_after" ]; then echo "  $t.dbc already installed"; continue; fi
  if [ "$cur" != "$want_before" ]; then echo "  REFUSE $t.dbc: live file changed since download ($cur)"; fail=1; continue; fi
  if [ "$new" != "$want_after" ]; then echo "  REFUSE $t.dbc: staged file is not the patched one ($new)"; fail=1; continue; fi
  echo "  $t.dbc $cur -> $new"
done
[ "$(md5sum < "$BASE/work/mmaps/15325235.mmtile" | cut -c1-12)" = "$CARVED_MD5" ] || { echo "  REFUSE: work/mmaps/15325235.mmtile is not the carved tile"; fail=1; }
for sub in maps vmaps mmaps; do
  for f in "$BASE/work/$sub"/1532*; do
    b=$(basename "$f")
    if [ -e "$DATA/$sub/$b" ] && ! cmp -s "$f" "$DATA/$sub/$b"; then echo "  REFUSE $sub/$b exists with other content"; fail=1; fi
  done
done
[ $fail = 0 ] || exit 1
echo "  map data: $(ls $BASE/work/maps/1532* | wc -l) maps, $(ls $BASE/work/vmaps/1532* | wc -l) vmaps, $(ls $BASE/work/mmaps/1532* | wc -l) mmaps"
[ "$MODE" = apply ] || { echo "check ok ($REALM)"; exit 0; }

for t in $TABLES; do
  live="$DATA/dbc/$t.dbc"; staged="$BASE/stage/dbc/$REALM/$t.dbc"
  want_after=$(awk -v f="$REALM/$t.dbc" '$2==f {print $1}' "$BASE/stage/after.md5")
  [ "$(md5sum < "$live" | cut -c1-32)" = "$want_after" ] && continue
  [ -e "$live.bak-chess-$TS" ] || cp -p "$live" "$live.bak-chess-$TS"
  cp "$staged" "$live.tmp-chess"
  mv -f "$live.tmp-chess" "$live"
  [ "$(md5sum < "$live" | cut -c1-32)" = "$want_after" ] || { echo "  $t.dbc md5 wrong after install"; exit 1; }
done
for sub in maps vmaps mmaps; do
  for f in "$BASE/work/$sub"/1532*; do
    b=$(basename "$f")
    [ -e "$DATA/$sub/$b" ] && continue
    cp "$f" "$DATA/$sub/.$b.tmp-chess"
    mv "$DATA/$sub/.$b.tmp-chess" "$DATA/$sub/$b"
  done
done
echo "installed ($REALM)"
for t in $TABLES; do md5sum "$DATA/dbc/$t.dbc" | cut -c1-12; done
echo "maps $(ls "$DATA"/maps/1532*.map | wc -l), vmaps $(ls "$DATA"/vmaps/1532* | wc -l), mmaps $(ls "$DATA"/mmaps/1532* | wc -l); hall tile $(md5sum < "$DATA/mmaps/15325235.mmtile" | cut -c1-12)"
