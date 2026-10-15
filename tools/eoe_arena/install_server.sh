#!/bin/bash
# The Eye of Eternity arena (map 1616): install the server side into one realm.
#   install_server.sh <tc-centurion|tc-centuriondev> check|apply
# DBCs: each live file must still be the one downloaded (md5 in before.md5), the
# staged one must be the patched one (after.md5); backups as <file>.bak-eoe-20261015.
# Terrain: map 1616's maps/vmaps/mmaps are new files; refuses to overwrite anything.
# A running worldserver reads DBCs only at startup and never loads map 1616 before
# its Map.dbc has the row, so none of this touches a running realm.
set -euo pipefail
REALM="$1"; MODE="$2"
BASE=/home/brokilodeluxe/eoe-arena-20261015
DATA=/home/brokilodeluxe/wow/servers/$REALM/data
TS=20261015
[ -d "$DATA/dbc" ] || { echo "no $DATA/dbc"; exit 1; }

fail=0
for t in Map BattlemasterList PvpDifficulty WorldSafeLocs WorldStateUI Light; do
  live="$DATA/dbc/$t.dbc"; staged="$BASE/dbc/$REALM/$t.dbc"
  want_before=$(awk -v f="$REALM/data/dbc/$t.dbc" '$2==f {print $1}' "$BASE/before.md5")
  want_after=$(awk -v f="$REALM/$t.dbc" '$2==f {print $1}' "$BASE/after.md5")
  cur=$(md5sum < "$live" | cut -c1-32); new=$(md5sum < "$staged" | cut -c1-32)
  if [ "$cur" = "$want_after" ]; then echo "  $t.dbc already installed"; continue; fi
  if [ "$cur" != "$want_before" ]; then echo "  REFUSE $t.dbc: live file changed since download ($cur)"; fail=1; continue; fi
  if [ "$new" != "$want_after" ]; then echo "  REFUSE $t.dbc: staged file is not the patched one ($new)"; fail=1; continue; fi
  echo "  $t.dbc $cur -> $new"
done
[ $fail = 0 ] || exit 1

for sub in maps vmaps mmaps; do
  for f in "$BASE/work/$sub"/1616*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    if [ -e "$DATA/$sub/$b" ]; then
      if cmp -s "$f" "$DATA/$sub/$b"; then continue; fi
      echo "  REFUSE $sub/$b exists with other content"; fail=1
    fi
  done
done
[ $fail = 0 ] || exit 1
echo "  terrain: $(ls $BASE/work/maps/1616* | wc -l) maps, $(ls $BASE/work/vmaps/1616* | wc -l) vmaps, $(ls $BASE/work/mmaps/1616* | wc -l) mmaps"
for m in Nexus_Raid_Floating_Platform.wmo.vmo Nexus_Floating_Platform_01_Small.wmo.vmo; do
  [ -f "$DATA/vmaps/$m" ] || { echo "  REFUSE: model $m missing in $DATA/vmaps"; exit 1; }
done

[ "$MODE" = apply ] || { echo "check ok ($REALM)"; exit 0; }

for t in Map BattlemasterList PvpDifficulty WorldSafeLocs WorldStateUI Light; do
  live="$DATA/dbc/$t.dbc"; staged="$BASE/dbc/$REALM/$t.dbc"
  want_after=$(awk -v f="$REALM/$t.dbc" '$2==f {print $1}' "$BASE/after.md5")
  [ "$(md5sum < "$live" | cut -c1-32)" = "$want_after" ] && continue
  [ -e "$live.bak-eoe-$TS" ] || cp -p "$live" "$live.bak-eoe-$TS"
  cp "$staged" "$live.tmp-eoe"
  mv -f "$live.tmp-eoe" "$live"
  [ "$(md5sum < "$live" | cut -c1-32)" = "$want_after" ] || { echo "  $t.dbc md5 wrong after install"; exit 1; }
done
for sub in maps vmaps mmaps; do
  for f in "$BASE/work/$sub"/1616*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    [ -e "$DATA/$sub/$b" ] && continue
    cp "$f" "$DATA/$sub/.$b.tmp-eoe"
    mv "$DATA/$sub/.$b.tmp-eoe" "$DATA/$sub/$b"
  done
done
echo "installed ($REALM)"
md5sum "$DATA"/dbc/{Map,BattlemasterList,PvpDifficulty,WorldSafeLocs,WorldStateUI,Light}.dbc
ls -la "$DATA"/vmaps/1616* "$DATA"/mmaps/1616* | awk '{print $5, $9}'
echo "maps: $(ls "$DATA"/maps/1616*.map | wc -l)"
