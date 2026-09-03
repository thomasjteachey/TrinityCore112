"""Make the SkillLineAbility mirror match the binary the server actually reads.

Direction is binary -> mirror, on purpose. The server reads data/dbc, so every
behaviour on the realm today already comes from the binary: importing it into
the mirror changes no gameplay at all and only makes the tooling honest. The
reverse would rewrite 1,358 rows of live behaviour, SupercededBySpell and
AcquireMethod among them, which is the pair that ate talent ranks once already.

What this replaces, measured rather than assumed:

    only in binary                 878   the mirror never received these
    only in mirror                   2   Perception 20600, Shadowmeld 20580
    shared id, different values   1358   mostly SupercededBySpell

The two mirror-only rows are REPORTED and dropped, not silently kept - they are
real racials, and their absence from the binary may be a separate bug. Keeping
them would leave the mirror still not equal to the binary, which is the whole
point of the exercise.

A timestamped backup table is made first, and the result is verified by reading
both sides back and comparing every field of every row.

    python3 sla_sync_to_mirror.py <Spell.dbc path> <table> [--apply]
"""
import struct, subprocess, sys, time

FIELDS = ["ID", "SkillLine", "Spell", "RaceMask", "ClassMask", "ExcludeRace",
          "ExcludeClass", "MinSkillLineRank", "SupercededBySpell", "AcquireMethod",
          "TrivialSkillLineRankHigh", "TrivialSkillLineRankLow",
          "CharacterPoints", "NumSkillUps"]

path, table = sys.argv[1], sys.argv[2]
apply = "--apply" in sys.argv

def sql(statement, db="dbc"):
    p = subprocess.run(
        ["mysql", "-u", "brokilodeluxe", "-N", "-D", db, "-e", statement],
        capture_output=True, text=True,
        env={"MYSQL_PWD": "TigerAss69?", "PATH": "/usr/bin:/bin"})
    if p.returncode:
        raise RuntimeError(p.stderr.strip())
    return p.stdout

d = open(path, "rb").read()
magic, n, fields, rsize, sblock = struct.unpack_from("<4sIIII", d, 0)
assert magic == b"WDBC", magic
assert fields == len(FIELDS), "field count %d, expected %d" % (fields, len(FIELDS))

binary = {}
for i in range(n):
    off = 20 + i * rsize
    r = struct.unpack_from("<%di" % fields, d, off)
    binary[r[0]] = r
print("binary rows: %d" % len(binary))

def load_mirror():
    out = sql("SELECT %s FROM %s;" % (", ".join(FIELDS), table))
    rows = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) == len(FIELDS):
            vals = tuple(int(p) for p in parts)
            rows[vals[0]] = vals
    return rows

before = load_mirror()
only_mirror = sorted(set(before) - set(binary))
print("mirror rows: %d" % len(before))
print("only in mirror (will be DROPPED): %d %s" % (len(only_mirror), only_mirror[:10]))

if not apply:
    print("dry run - rerun with --apply")
    raise SystemExit(0)

stamp = time.strftime("%Y%m%d_%H%M%S")
backup = "%s_bak_parity_%s" % (table, stamp)
sql("CREATE TABLE %s LIKE %s;" % (backup, table))
sql("INSERT INTO %s SELECT * FROM %s;" % (backup, table))
kept = int(sql("SELECT COUNT(*) FROM %s;" % backup).strip())
print("backup %s holds %d rows" % (backup, kept))
assert kept == len(before), "backup did not capture every row"

sql("DELETE FROM %s;" % table)

cols = ", ".join(FIELDS)
values, batch = [], 0
for rid in sorted(binary):
    values.append("(" + ",".join(str(v) for v in binary[rid]) + ")")
    if len(values) >= 500:
        sql("INSERT INTO %s (%s) VALUES %s;" % (table, cols, ",".join(values)))
        batch += len(values)
        values = []
if values:
    sql("INSERT INTO %s (%s) VALUES %s;" % (table, cols, ",".join(values)))
    batch += len(values)
print("inserted %d rows" % batch)

after = load_mirror()
missing = sorted(set(binary) - set(after))
extra = sorted(set(after) - set(binary))
mismatch = [rid for rid in set(binary) & set(after) if binary[rid] != after[rid]]
print("VERIFY  mirror=%d  missing=%d  extra=%d  value mismatches=%d"
      % (len(after), len(missing), len(extra), len(mismatch)))
print("PARITY: %s" % ("EXACT" if not (missing or extra or mismatch) else "FAILED"))
