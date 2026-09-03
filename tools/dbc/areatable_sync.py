"""Make the AreaTable mirror match the binary the server reads.

Same direction and same reasoning as the SkillLineAbility sync: the server runs
on data/dbc, so the binary is what every observable behaviour already comes
from. Importing it into the mirror changes no gameplay and only makes the
tooling honest.

AreaTable is simpler than SkillLineAbility turned out to be - the mirror is a
strict SUBSET here, 27 rows behind with nothing of its own - but it is fiddlier
to write, because 16 of its 36 fields are localized STRINGS held as offsets into
the block after the records, not integers. Fields 33 and 34 are floats.

A full replace rather than an insert of the 27, so that any value drift on the
2,326 shared rows is corrected too rather than left to be discovered later.

    python3 areatable_sync.py <AreaTable.dbc> <table> [--apply]
"""
import struct, subprocess, sys, time

COLS = ["ID", "ContinentID", "ParentAreaID", "AreaBit", "Flags",
        "SoundProviderPref", "SoundProviderPrefUnderwater", "AmbienceID",
        "ZoneMusic", "IntroSound", "ExplorationLevel"] + \
       ["AreaName_Lang_%s" % s for s in
        ["enUS", "enGB", "koKR", "frFR", "deDE", "enCN", "zhCN", "enTW", "zhTW",
         "esES", "esMX", "ruRU", "ptPT", "ptBR", "itIT", "Unk"]] + \
       ["AreaName_Lang_Mask", "FactionGroupMask",
        "LiquidTypeID_1", "LiquidTypeID_2", "LiquidTypeID_3", "LiquidTypeID_4",
        "MinElevation", "Ambient_Multiplier", "Lightid"]

STRING_FIELDS = set(range(11, 27))     # the 16 locale names
FLOAT_FIELDS = {33, 34}

path, table = sys.argv[1], sys.argv[2]
apply = "--apply" in sys.argv

def sql(statement):
    p = subprocess.run(["mysql", "-u", "brokilodeluxe", "-N", "-D", "dbc", "-e", statement],
                       capture_output=True, text=True,
                       env={"MYSQL_PWD": "TigerAss69?", "PATH": "/usr/bin:/bin"})
    if p.returncode:
        raise RuntimeError(p.stderr.strip()[:400])
    return p.stdout

d = open(path, "rb").read()
magic, n, fields, rsize, sblock = struct.unpack_from("<4sIIII", d, 0)
assert magic == b"WDBC", magic
assert fields == len(COLS), "binary has %d fields, mirror has %d columns" % (fields, len(COLS))
sbase = 20 + n * rsize

def cstr(off):
    if off == 0:
        return ""
    end = d.index(b"\0", sbase + off)
    return d[sbase + off:end].decode("utf-8", "replace")

rows = []
for i in range(n):
    off = 20 + i * rsize
    raw = struct.unpack_from("<%di" % fields, d, off)
    vals = []
    for f in range(fields):
        if f in STRING_FIELDS:
            vals.append(cstr(raw[f] & 0xFFFFFFFF))
        elif f in FLOAT_FIELDS:
            vals.append(struct.unpack_from("<f", d, off + f * 4)[0])
        else:
            vals.append(raw[f])
    rows.append(vals)
print("binary rows: %d" % len(rows))

mirror_count = int(sql("SELECT COUNT(*) FROM %s;" % table).strip())
print("mirror rows: %d" % mirror_count)

if not apply:
    print("dry run - rerun with --apply")
    raise SystemExit(0)

stamp = time.strftime("%Y%m%d_%H%M%S")
backup = "%s_bak_parity_%s" % (table, stamp)
sql("CREATE TABLE %s LIKE %s;" % (backup, table))
sql("INSERT INTO %s SELECT * FROM %s;" % (backup, table))
kept = int(sql("SELECT COUNT(*) FROM %s;" % backup).strip())
print("backup %s holds %d rows" % (backup, kept))
assert kept == mirror_count, "backup did not capture every row"

def lit(f, v):
    if f in STRING_FIELDS:
        return "'" + v.replace("\\", "\\\\").replace("'", "\\'") + "'"
    if f in FLOAT_FIELDS:
        return repr(float(v))
    return str(int(v))

sql("DELETE FROM %s;" % table)
colnames = ", ".join(COLS)
buf, total = [], 0
for vals in rows:
    buf.append("(" + ",".join(lit(f, vals[f]) for f in range(fields)) + ")")
    if len(buf) >= 200:
        sql("INSERT INTO %s (%s) VALUES %s;" % (table, colnames, ",".join(buf)))
        total += len(buf); buf = []
if buf:
    sql("INSERT INTO %s (%s) VALUES %s;" % (table, colnames, ",".join(buf)))
    total += len(buf)
print("inserted %d rows" % total)

after = int(sql("SELECT COUNT(*) FROM %s;" % table).strip())
sample = sql("SELECT ID, AreaName_Lang_enUS, Flags FROM %s WHERE ID IN (30232, 2177, 33) ORDER BY ID;" % table)
print("VERIFY mirror=%d (binary=%d) -> %s" % (after, len(rows), "MATCH" if after == len(rows) else "MISMATCH"))
print("spot check:")
for line in sample.splitlines():
    print("   " + line)
