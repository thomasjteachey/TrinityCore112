#!/bin/bash
# Publish Blood Beneath the Arena's client files into patch-enUS-A (Centurion's DBC patch).
#   bash bba_publish_a.sh check     - build + verify the new archive, publish nothing
#   bash bba_publish_a.sh publish   - same, then back up, swap and bump .version
#
# Adds:  World/Maps/BloodBeneathArena/BloodBeneathArena.wdt + _31_56.adt (new files)
# Edits (the ARCHIVE'S OWN copies, rows appended by bba_dbc.py):
#        DBFilesClient/Map.dbc, AreaTable.dbc, MapDifficulty.dbc, Item.dbc
set -euo pipefail
MODE="${1:-check}"
P=/var/www/html/downloads/patches
W=/tmp/bba/pub
EXPECT_ZIP_MD5=318bfbab6564bd91b8d511fec200b66c   # live zip the DBC copies were extracted from
OLDVER=$(cat $P/patch-enUS-A.version)

cur=$(md5sum $P/patch-enUS-A.zip | cut -d' ' -f1)
[ "$cur" = "$EXPECT_ZIP_MD5" ] || { echo "live patch-enUS-A.zip changed since extraction ($cur) - re-extract first"; exit 1; }

rm -rf $W && mkdir -p $W/stage && cd $W
unzip -q $P/patch-enUS-A.zip
[ -f patch-enUS-A.MPQ ] || { echo "no inner MPQ"; exit 1; }
fv=$(od -An -tu2 -j12 -N2 patch-enUS-A.MPQ | tr -d ' ')
echo "formatVersion before: $fv"
nfiles_before=$(smpq -l patch-enUS-A.MPQ | wc -l)

# DBCs: fresh extraction of this archive's own copies, rows appended
mkdir -p stage/DBFilesClient
( cd stage && for f in Map AreaTable MapDifficulty Item; do smpq -x ../patch-enUS-A.MPQ DBFilesClient/$f.dbc; done )
python3 /tmp/bba/bba_dbc.py $W/stage/DBFilesClient
rm -f stage/DBFilesClient/*.bak-bba
mkdir -p stage/World/Maps/BloodBeneathArena
cp /tmp/bba/stage/World/Maps/BloodBeneathArena/* stage/World/Maps/BloodBeneathArena/

FILES="DBFilesClient/Map.dbc DBFilesClient/AreaTable.dbc DBFilesClient/MapDifficulty.dbc DBFilesClient/Item.dbc World/Maps/BloodBeneathArena/BloodBeneathArena.wdt World/Maps/BloodBeneathArena/BloodBeneathArena_31_56.adt"
( cd stage && smpq -a -f ../patch-enUS-A.MPQ $FILES )

fv2=$(od -An -tu2 -j12 -N2 patch-enUS-A.MPQ | tr -d ' ')
[ "$fv2" = "$fv" ] || { echo "formatVersion changed $fv -> $fv2"; exit 1; }
nfiles_after=$(smpq -l patch-enUS-A.MPQ | wc -l)
echo "files in archive: $nfiles_before -> $nfiles_after"

# read every file back out of the rebuilt archive and compare
mkdir -p back && ( cd back && for f in $FILES; do smpq -x ../patch-enUS-A.MPQ "$f"; done )
for f in $FILES; do
  a=$(md5sum "stage/$f" | cut -d' ' -f1); b=$(md5sum "back/$f" | cut -d' ' -f1)
  [ "$a" = "$b" ] || { echo "READBACK MISMATCH $f"; exit 1; }
  echo "readback ok  $f  $a"
done

zip -q patch-enUS-A.zip.new patch-enUS-A.MPQ
unzip -v patch-enUS-A.zip.new | tail -3
NEWVER=$(python3 -c "v='$OLDVER'; a,b=v.split('.'); print('%s.%05d' % (a, int(b)+1))")
echo "version $OLDVER -> $NEWVER"

if [ "$MODE" != publish ]; then echo "check only - nothing published"; exit 0; fi

cp -p $P/patch-enUS-A.zip $P/patch-enUS-A.zip.bak-bba-$OLDVER
cp -p $P/patch-enUS-A.version $P/patch-enUS-A.version.bak-bba-$OLDVER
cp patch-enUS-A.zip.new $P/.patch-enUS-A.zip.tmp
mv $P/.patch-enUS-A.zip.tmp $P/patch-enUS-A.zip
printf '%s' "$NEWVER" > $P/patch-enUS-A.version
echo "published patch-enUS-A $NEWVER"

# fetch it back over HTTP and verify the files inside what the webserver serves
mkdir -p http && cd http
curl -s -o a.zip http://127.0.0.1/downloads/patches/patch-enUS-A.zip
curl -s http://127.0.0.1/downloads/patches/patch-enUS-A.version; echo
unzip -q a.zip && mkdir -p x && ( cd x && for f in $FILES; do smpq -x ../patch-enUS-A.MPQ "$f"; done )
for f in $FILES; do
  a=$(md5sum "$W/stage/$f" | cut -d' ' -f1); b=$(md5sum "x/$f" | cut -d' ' -f1)
  [ "$a" = "$b" ] && echo "http ok  $f" || echo "HTTP MISMATCH $f"
done
