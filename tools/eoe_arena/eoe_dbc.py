"""The Eye of Eternity arena: its DBC rows.

    python eoe_dbc.py show                          # print every row, no writes
    python eoe_dbc.py apply <dbc-dir> [--only Map,Light] [--dry-run]
    python eoe_dbc.py verify <dbc-dir> [--only ...]

Rows are templated from Centurion's own client copies (the highest-priority
archive in the local client, patch-enUS-A for all of these), so every layer -
both servers' data/dbc, patch-enUS-A, the HD World pack's Light.dbc - gets the
same bytes:

    Map 1616               616's row as an arena: InstanceType 4, flags 1, no
                           corpse map, expansion 0 (the realm is expansion
                           locked), Directory EyeOfEternityArena, its own
                           loading screen (236).
    BattlemasterList 886   The Twisting Nether's (881) shape.
    PvpDifficulty          sixteen 5-level brackets, 10-89, like every arena.
    WorldSafeLocs          the two pad starts.
    WorldStateUI           the "N Players Remaining" pair, The Twisting Nether's.
    Light 3616             the Eye's own map-wide light (1773) for map 1616: the
                           NexusRaid_StarrySky skybox. Without a Light row a map
                           falls back to the default daytime sky.
    MapDifficulty 760      server only: (1616, difficulty 0), the row stock
                           Dalaran Sewers (617) has. ObjectMgr allows a map only
                           the spawn modes MapDifficulty lists, so without it every
                           static `gameobject` row on 1616 (the pillars) is skipped
                           as "wrong spawn mask". An arena runs in its PvpDifficulty
                           bracket's difficulty, 0. The client never needs it for an
                           arena, so eoe_patch.py does not ship it.

`apply` only touches files that exist in the directory (a patch that does not
carry a table is not given one), backs each one up once as <file>.bak-eoe, and
is idempotent. A row whose id is already present with other values stops the
run - every id here is meant to be new.
"""

import argparse
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layout as L  # noqa: E402
from eoe_terrain import resolve  # noqa: E402

BACKUP_SUFFIX = ".bak-eoe"


def loc(first, count=16):
    return list(range(first, first + count))


# String field indexes per table (3.3.5a 12340 layouts; field counts are checked
# against each file before anything is written).
SPECS = {
    "Map": (66, [1] + loc(5) + loc(23) + loc(40)),
    "BattlemasterList": (32, loc(11)),
    "PvpDifficulty": (6, []),
    "WorldSafeLocs": (22, loc(5)),
    "WorldStateUI": (63, [4] + loc(5) + loc(22) + [41] + loc(42) + [59]),
    "Light": (15, []),
    "MapDifficulty": (23, loc(3) + [22]),
}


class Dbc:
    def __init__(self, blob, name):
        magic, self.count, self.fields, self.rec_size, self.str_size = struct.unpack_from("<4sIIII", blob, 0)
        if magic != b"WDBC":
            raise ValueError("%s: not a WDBC file" % name)
        self.name = name
        self.records = bytearray(blob[20:20 + self.count * self.rec_size])
        self.strings = bytearray(blob[20 + self.count * self.rec_size:])
        self.index = {}
        for i in range(self.count):
            self.index[struct.unpack_from("<I", self.records, i * self.rec_size)[0]] = i

    def raw(self, rid):
        i = self.index[rid]
        return list(struct.unpack_from("<%dI" % self.fields, self.records, i * self.rec_size))

    def string(self, off):
        if off == 0 or off >= len(self.strings):
            return ""
        end = self.strings.index(b"\0", off)
        return self.strings[off:end].decode("utf-8")

    def row(self, rid, string_fields):
        values = self.raw(rid)
        return [self.string(v) if f in string_fields else v for f, v in enumerate(values)]

    def blob(self):
        return struct.pack("<4sIIII", b"WDBC", self.count, self.fields, self.rec_size, len(self.strings)) \
            + bytes(self.records) + bytes(self.strings)


_client = {}


def client_row(name, rid, **changes):
    """A row from the client's own copy, with field changes (f<index>=value)."""
    if name not in _client:
        arc, blob = resolve("DBFilesClient\\%s.dbc" % name)
        _client[name] = Dbc(blob, name)
    row = _client[name].row(rid, set(SPECS[name][1]))
    for field, value in changes.items():
        row[int(field.lstrip("f"))] = value
    return row


def f32(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def build_rows():
    rows = {name: [] for name in SPECS}

    rows["Map"].append(client_row(
        "Map", L.SOURCE_MAP_ID,
        f0=L.MAP_ID, f1=L.DIRECTORY,
        f2=4,                 # InstanceType: arena (the client's arena UI keys off this)
        f3=1,                 # Flags, as every arena here
        f4=0,                 # PVP
        f22=L.AREA_ID,        # AreaTableID: the zone the terrain reports
        f57=L.LOADING_SCREEN,
        f58=f32(1.0),         # MinimapIconScale
        f59=0xFFFFFFFF,       # CorpseMapID: none
        f60=0, f61=0,         # Corpse X/Y
        f62=0xFFFFFFFF,       # TimeOfDayOverride: none
        f63=0,                # ExpansionID
        f64=0, f65=0))        # RaidOffset, MaxPlayers

    rows["BattlemasterList"].append(client_row(
        "BattlemasterList", 881, f0=L.BG_TYPE_ID, f1=L.MAP_ID, f11=L.NAME))

    for bracket in range(16):
        low = 10 + 5 * bracket
        rows["PvpDifficulty"].append([L.PVPDIFF_FIRST + bracket, L.MAP_ID, bracket, low, low + 4, 0])

    for rid, team, start in ((L.WSL_ALLIANCE, "Alliance", L.ALLIANCE_START), (L.WSL_HORDE, "Horde", L.HORDE_START)):
        rows["WorldSafeLocs"].append(client_row(
            "WorldSafeLocs", 52618, f0=rid, f1=L.MAP_ID,
            f2=f32(start[0]), f3=f32(start[1]), f4=f32(start[2]),
            f5="%s - %s Start" % (L.NAME, team)))

    rows["WorldStateUI"].append(client_row("WorldStateUI", 90118, f0=L.WSUI_GREEN, f1=L.MAP_ID))
    rows["WorldStateUI"].append(client_row("WorldStateUI", 90119, f0=L.WSUI_GOLD, f1=L.MAP_ID))

    rows["Light"].append(client_row("Light", L.LIGHT_SOURCE, f0=L.LIGHT_ID, f1=L.MAP_ID))

    rows["MapDifficulty"].append(client_row("MapDifficulty", 112, f0=L.MAPDIFF_ID, f1=L.MAP_ID))
    return rows


# ------------------------------------------------------------------ writer
def encode(name, row):
    nfields, strings = SPECS[name]
    if len(row) != nfields:
        raise ValueError("%s row %s has %d fields, need %d" % (name, row[0], len(row), nfields))
    out = []
    for f, value in enumerate(row):
        if f in strings:
            if not isinstance(value, str):
                raise ValueError("%s row %s field %d must be a string" % (name, row[0], f))
            out.append(value)
        elif isinstance(value, float):
            out.append(f32(value))
        elif isinstance(value, str):
            raise ValueError("%s row %s field %d is not a string field" % (name, row[0], f))
        else:
            out.append(value & 0xFFFFFFFF)
    return out


def intern(dbc, text, pool):
    if text == "":
        return 0
    if text not in pool:
        pool[text] = len(dbc.strings)
        dbc.strings += text.encode("utf-8") + b"\0"
    return pool[text]


def upsert(path, name, rows, dry_run):
    dbc = Dbc(open(path, "rb").read(), name)
    nfields, strings = SPECS[name]
    if dbc.fields != nfields or dbc.rec_size != nfields * 4:
        raise SystemExit("%s: %d fields / %d bytes, expected %d" % (path, dbc.fields, dbc.rec_size, nfields))
    pool = {}
    added = same = 0
    for row in rows:
        values = encode(name, row)
        rid = values[0]
        if rid in dbc.index:
            if dbc.row(rid, set(strings)) == values:
                same += 1
                continue
            raise SystemExit("%s: row %d exists with different values - id collision" % (path, rid))
        packed = [intern(dbc, v, pool) if f in strings else v for f, v in enumerate(values)]
        dbc.records += struct.pack("<%dI" % nfields, *packed)
        dbc.index[rid] = dbc.count
        dbc.count += 1
        added += 1
    print("    %-22s +%d added, %d already right  (%d rows)" % (name + ".dbc", added, same, dbc.count))
    if dry_run or not added:
        return
    backup = path + BACKUP_SUFFIX
    if not os.path.exists(backup):
        shutil.copy2(path, backup)
    tmp = path + ".tmp-eoe"
    with open(tmp, "wb") as fh:
        fh.write(dbc.blob())
    os.replace(tmp, path)


def verify(path, name, rows):
    dbc = Dbc(open(path, "rb").read(), name)
    strings = set(SPECS[name][1])
    bad = 0
    for row in rows:
        values = encode(name, row)
        if values[0] not in dbc.index:
            print("    %s: row %d MISSING" % (name, values[0]))
            bad += 1
        elif dbc.row(values[0], strings) != values:
            print("    %s: row %d DIFFERS" % (name, values[0]))
            bad += 1
    print("    %-22s %s" % (name + ".dbc", "ok (%d rows)" % len(rows) if not bad else "%d bad" % bad))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=("show", "apply", "verify"))
    ap.add_argument("dir", nargs="?")
    ap.add_argument("--only", help="comma-separated table names")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    rows = build_rows()
    names = [n for n in SPECS if not args.only or n in args.only.split(",")]

    if args.command == "show":
        for name in names:
            for row in rows[name]:
                print(name, [v if not isinstance(v, str) else repr(v) for v in row if v not in ("", 0)])
        return 0

    if not args.dir or not os.path.isdir(args.dir):
        ap.error("need a DBC directory")
    print(args.dir)
    bad = 0
    for name in names:
        path = os.path.join(args.dir, name + ".dbc")
        if not os.path.exists(path):
            print("    %-22s not present here, skipped" % (name + ".dbc"))
            continue
        if args.command == "apply":
            upsert(path, name, rows[name], args.dry_run)
        else:
            bad += verify(path, name, rows[name])
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
