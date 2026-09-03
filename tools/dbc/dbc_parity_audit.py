"""Row-count parity between each binary .dbc the server reads and its mirror.

The server reads data/dbc/*.dbc. The mirrors in the `dbc` schema are what
dbcgen regenerates FROM, and what most tooling queries. Where they disagree,
one of two things is true and both are worth knowing: either the mirror is
stale (tooling is answering questions about a file the server does not have),
or the binary is stale (an edit never reached the server).

Row count is a cheap first pass, not proof of equality - two files can have the
same count and different content. It is enough to find which tables need a
closer look.
"""
import os, re, struct, subprocess, sys

REALM_DIR = sys.argv[1] if len(sys.argv) > 1 else \
    "/home/brokilodeluxe/wow/servers/tc-barracksplus/data/dbc"
SUFFIX = sys.argv[2] if len(sys.argv) > 2 else "bplus"

def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout

tables = [t for t in sh(
    "MYSQL_PWD='TigerAss69?' mysql -u brokilodeluxe -N -e "
    "\"SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA='dbc' "
    "AND TABLE_NAME LIKE '%%\\\\_%s';\"" % SUFFIX).split() if t]

# stem -> actual filename on disk, matched case-insensitively
files = {}
for fn in os.listdir(REALM_DIR):
    if fn.lower().endswith(".dbc"):
        files[fn[:-4].lower()] = fn

def dbc_rows(path):
    with open(path, "rb") as f:
        head = f.read(20)
    magic, n, fields, rsize, sblock = struct.unpack("<4sIIII", head)
    if magic != b"WDBC":
        return None
    return n

rows = []
for table in sorted(tables):
    # skip the obvious backups rather than reporting a hundred of them
    stem = table[:-(len(SUFFIX) + 1)]
    if re.search(r"_(bak|backup)", stem):
        continue
    fn = files.get(stem.lower())
    if not fn:
        rows.append((table, None, None, "no matching .dbc"))
        continue
    binary = dbc_rows(os.path.join(REALM_DIR, fn))
    out = sh("MYSQL_PWD='TigerAss69?' mysql -u brokilodeluxe -N -D dbc -e "
             "'SELECT COUNT(*) FROM %s;'" % table).strip()
    mirror = int(out) if out.isdigit() else None
    if binary is None or mirror is None:
        rows.append((table, binary, mirror, "unreadable"))
    elif binary == mirror:
        rows.append((table, binary, mirror, "ok"))
    else:
        rows.append((table, binary, mirror, "DRIFT %+d" % (mirror - binary)))

print("%-34s %10s %10s   %s" % ("mirror table", "binary", "mirror", "state"))
print("-" * 78)
for table, b, m, state in rows:
    print("%-34s %10s %10s   %s" % (table, b if b is not None else "-",
                                    m if m is not None else "-", state))

drift = [r for r in rows if r[3].startswith("DRIFT")]
print("\n%d table(s) compared, %d in drift" % (len(rows), len(drift)))
