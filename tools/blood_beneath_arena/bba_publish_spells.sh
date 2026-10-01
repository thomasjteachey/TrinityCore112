#!/bin/bash
# Publish the boon spells (92100-92107) and named abilities (92110-92151) into patch-enUS-A's OWN Spell.dbc.
#   bash bba_publish_spells.sh <expected current version> check|publish
# Refuses if patch-enUS-A moved past the expected version (someone else published).
set -euo pipefail
EXPECT="$1"; MODE="${2:-check}"
P=/var/www/html/downloads/patches
W=/tmp/bba/pubspell
OLDVER=$(cat $P/patch-enUS-A.version)
[ "$OLDVER" = "$EXPECT" ] || { echo "patch-enUS-A is $OLDVER, expected $EXPECT - re-check before publishing"; exit 1; }

rm -rf $W && mkdir -p $W/stage/DBFilesClient && cd $W
unzip -q $P/patch-enUS-A.zip
fv=$(od -An -tu2 -j12 -N2 patch-enUS-A.MPQ | tr -d ' ')
( cd stage && smpq -x ../patch-enUS-A.MPQ DBFilesClient/Spell.dbc )
python3 /tmp/bba/bba_spells.py $W/stage/DBFilesClient
python3 /tmp/bba/bba_ability_spells.py $W/stage/DBFilesClient
rm -f stage/DBFilesClient/*.bak-bba*
( cd stage && smpq -a -f ../patch-enUS-A.MPQ DBFilesClient/Spell.dbc )
fv2=$(od -An -tu2 -j12 -N2 patch-enUS-A.MPQ | tr -d ' ')
[ "$fv2" = "$fv" ] || { echo "formatVersion changed $fv -> $fv2"; exit 1; }
mkdir -p back && ( cd back && smpq -x ../patch-enUS-A.MPQ DBFilesClient/Spell.dbc )
a=$(md5sum stage/DBFilesClient/Spell.dbc | cut -d' ' -f1); b=$(md5sum back/DBFilesClient/Spell.dbc | cut -d' ' -f1)
[ "$a" = "$b" ] || { echo "READBACK MISMATCH"; exit 1; }
echo "readback ok Spell.dbc $a"
zip -q patch-enUS-A.zip.new patch-enUS-A.MPQ
NEWVER=$(python3 -c "v='$OLDVER'; x,y=v.split('.'); print('%s.%05d' % (x, int(y)+1))")
echo "version $OLDVER -> $NEWVER"
[ "$MODE" = publish ] || { echo "check only - nothing published"; exit 0; }

cp -p $P/patch-enUS-A.zip $P/patch-enUS-A.zip.bak-bbaspells-$OLDVER
cp -p $P/patch-enUS-A.version $P/patch-enUS-A.version.bak-bbaspells-$OLDVER
cp patch-enUS-A.zip.new $P/.patch-enUS-A.zip.tmp
mv $P/.patch-enUS-A.zip.tmp $P/patch-enUS-A.zip
printf '%s' "$NEWVER" > $P/patch-enUS-A.version
echo "published patch-enUS-A $NEWVER"
mkdir -p http && cd http
curl -s -o a.zip http://127.0.0.1/downloads/patches/patch-enUS-A.zip
unzip -q a.zip && smpq -x patch-enUS-A.MPQ DBFilesClient/Spell.dbc
c=$(md5sum DBFilesClient/Spell.dbc | cut -d' ' -f1)
[ "$c" = "$a" ] && echo "http ok Spell.dbc" || echo "HTTP MISMATCH"
for f in DBFilesClient/Map.dbc World/Maps/BloodBeneathArena/BloodBeneathArena_31_56.adt; do smpq -x patch-enUS-A.MPQ "$f" && echo "still present: $f"; done
