#!/bin/bash
# The Gamesman's Hall, second layout (2026-10-15: bookcases in the balcony windows, doors in the
# side arches, no start teleport, no corner portcullises): install its server side into one realm.
#   install_gates.sh <tc-centurion|tc-centuriondev> check|apply
#
#   dbc/GameObjectDisplayInfo.dbc   + 11973, the bookcase (chess_dbc.py): the live file must still
#                                   be the one downloaded (stage/before.md5), the staged one the
#                                   patched one (stage/after.md5)
#   vmaps/KarazahnBookshelfXtralarge_GO.m2.vmo + its GameObjectModels.dtree entry (11973,
#                                   isWmo 0; tools/tanaris/build_go_vmo.py --doodad), so spells,
#                                   Blink and charges see the bookcases
#   mmaps/15325235.mmtile           the hall tile carved for the statues, the hall doors and the
#                                   bookcases (chess_nav.py); the live one must be the first carve
#
# Backups as <file>.bak-gates-20261015. The worldserver reads the DBC and the dtree at startup
# only, and a tile when a match first loads map 1532, so this changes nothing in a running realm
# that the world rows (gen_chess_sql.py, applied at the restart) do not also need.
set -euo pipefail
REALM="$1"; MODE="$2"
BASE=/home/brokilodeluxe/gmh-20261015
DATA=/home/brokilodeluxe/wow/servers/$REALM/data
TAG=bak-gates-20261015
OLD_TILE=1adb8e13283d          # the first carve (portcullises, doors, statues)
NEW_TILE=7d0d9da530cd          # this one (doors, statues, bookcases)
VMO=KarazahnBookshelfXtralarge_GO.m2
[ -d "$DATA/dbc" ] || { echo "no $DATA/dbc"; exit 1; }

fail=0
live="$DATA/dbc/GameObjectDisplayInfo.dbc"; staged="$BASE/stage/$REALM/GameObjectDisplayInfo.dbc"
want_before=$(awk -v f="$REALM/GameObjectDisplayInfo.dbc" '$2==f {print $1}' "$BASE/stage/before.md5")
want_after=$(awk -v f="$REALM/GameObjectDisplayInfo.dbc" '$2==f {print $1}' "$BASE/stage/after.md5")
cur=$(md5sum < "$live" | cut -c1-32); new=$(md5sum < "$staged" | cut -c1-32)
dbc_todo=1
if [ "$cur" = "$want_after" ]; then echo "  GameObjectDisplayInfo.dbc already installed"; dbc_todo=0
elif [ "$cur" != "$want_before" ]; then echo "  REFUSE GameObjectDisplayInfo.dbc: live file changed since download ($cur)"; fail=1
elif [ "$new" != "$want_after" ]; then echo "  REFUSE GameObjectDisplayInfo.dbc: staged file is not the patched one ($new)"; fail=1
else echo "  GameObjectDisplayInfo.dbc $cur -> $new"; fi

tile="$DATA/mmaps/15325235.mmtile"
cur_tile=$(md5sum < "$tile" | cut -c1-12)
[ "$(md5sum < "$BASE/carve/mmaps/15325235.mmtile" | cut -c1-12)" = "$NEW_TILE" ] || { echo "  REFUSE: carve/mmaps/15325235.mmtile is not the new tile"; fail=1; }
tile_todo=1
if [ "$cur_tile" = "$NEW_TILE" ]; then echo "  15325235.mmtile already installed"; tile_todo=0
elif [ "$cur_tile" != "$OLD_TILE" ]; then echo "  REFUSE 15325235.mmtile: live tile is neither carve ($cur_tile)"; fail=1
else echo "  15325235.mmtile $cur_tile -> $NEW_TILE"; fi

if grep -q "$VMO" "$DATA/vmaps/GameObjectModels.dtree" && [ -e "$DATA/vmaps/$VMO.vmo" ]; then
  echo "  $VMO.vmo and its dtree entry already installed"; vmo_todo=0
else
  echo "  $VMO.vmo + dtree entry for 11973 to add"; vmo_todo=1
fi
[ $fail = 0 ] || exit 1
[ "$MODE" = apply ] || { echo "check ok ($REALM)"; exit 0; }

if [ $dbc_todo = 1 ]; then
  [ -e "$live.$TAG" ] || cp -p "$live" "$live.$TAG"
  cp "$staged" "$live.tmp-gates"; mv -f "$live.tmp-gates" "$live"
  [ "$(md5sum < "$live" | cut -c1-32)" = "$want_after" ] || { echo "  GameObjectDisplayInfo.dbc md5 wrong after install"; exit 1; }
fi
if [ $vmo_todo = 1 ]; then
  dtree="$DATA/vmaps/GameObjectModels.dtree"
  [ -e "$dtree.$TAG" ] || cp -p "$dtree" "$dtree.$TAG"
  python3 "$BASE/build_go_vmo.py" --doodad "$BASE/KarazahnBookshelfXtralarge.m2" "$VMO" "$dtree" "$DATA/vmaps" 11973
fi
if [ $tile_todo = 1 ]; then
  [ -e "$tile.$TAG" ] || cp -p "$tile" "$tile.$TAG"
  cp "$BASE/carve/mmaps/15325235.mmtile" "$tile.tmp-gates"; mv -f "$tile.tmp-gates" "$tile"
  [ "$(md5sum < "$tile" | cut -c1-12)" = "$NEW_TILE" ] || { echo "  15325235.mmtile md5 wrong after install"; exit 1; }
fi
echo "installed ($REALM): GameObjectDisplayInfo $(md5sum < "$live" | cut -c1-12), tile $(md5sum < "$tile" | cut -c1-12), dtree $(grep -c "$VMO" "$DATA/vmaps/GameObjectModels.dtree") entry"
