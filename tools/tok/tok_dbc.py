"""Temple of Kotmogu: the DBC rows, built from the Ascension client data.

Every row here is either copied from Ascension's own DBCs (read straight out of
its MPQs, so this is reproducible) or written out in full where Ascension had no
row to copy. The battleground is type 106 on Ascension's map id 998.

    python tok_dbc.py show                          # print every row, no writes
    python tok_dbc.py apply <dbc-dir> [--only Map,Spell] [--dry-run]
    python tok_dbc.py verify <dbc-dir> [--only ...]

`apply` works on a directory of binary DBCs: a server's data/dbc, or the files
extracted out of a client patch. It only touches files that exist there (a patch
that does not carry a DBC is not given one; seed it first if it needs it), backs
each file up once as <file>.bak-tok, and is idempotent: a row already present
with the same values is left alone. One present with DIFFERENT values stops the
run, since every id here is meant to be new; --rewrite replaces it in place
(strings are appended to the block, never patched over).

Ids, and why:
    Map 998, AreaTable 6051/6060/6061/6136 - the ids the terrain was authored
        with. The area ids are baked into every MCNK chunk, so keeping them means
        no ADT has to be rewritten. The AreaBits are NOT kept: Ascension's
        4490-4493 overflow the client's 4096-bit explored-zone field.
    BattlemasterList 106 - must equal the battleground type id, which must stay
        below 256 (BattlegroundMgr::ScheduleQueueUpdate packs it in a byte), so
        Ascension's 790 cannot be used.
    WorldSafeLocs 52700-52705, WorldStateUI 90028/90029, WorldMapArea 9534,
        PvpDifficulty 90998 + 93239-93243 - this realm's custom blocks.
    Spell 92300-92307 - Ascension's 8445xx orb auras, renumbered.
    SpellIcon/SpellVisual/SpellVisualKit/SpellVisualKitModelAttach/
        SpellVisualEffectName, LoadingScreens 334, GameObjectDisplayInfo 11533 -
        Ascension's ids, verified free in every copy this realm ships.
    GameObjectDisplayInfo 11960-11963 - the coloured beams. Ascension only has
        these models as creature displays, so the rows are new.
"""

import argparse
import os
import shutil
import struct
import sys

ASC_TOOLS = r"C:\Ascension\ExtractedMaps\_tools"
ASC_DATA = r"C:\Ascension\Launcher\resources\ascension-live\Data"
BACKUP_SUFFIX = ".bak-tok"


def loc(first, count=16):
    return list(range(first, first + count))


# String field indexes per DBC (3.3.5a 12340 layouts; field counts checked
# against the file before anything is written).
SPECS = {
    "Map": (66, [1] + loc(5) + loc(23) + loc(40)),
    "AreaTable": (36, loc(11)),
    "BattlemasterList": (32, loc(11)),
    "PvpDifficulty": (6, []),
    "WorldSafeLocs": (22, loc(5)),
    "WorldStateUI": (63, [4] + loc(5) + loc(22) + [41] + loc(42) + [59]),
    "WorldMapArea": (11, [3]),
    "LoadingScreens": (4, [1, 2]),
    "Spell": (234, loc(136) + loc(153) + loc(170) + loc(187)),
    "SpellIcon": (2, [1]),
    "SpellVisual": (32, []),
    "SpellVisualKit": (38, []),
    "SpellVisualKitModelAttach": (10, []),
    "SpellVisualEffectName": (7, [1, 2]),
    "GameObjectDisplayInfo": (19, [1]),
}

# Where each source table lives in Ascension's archives.
ASC_SOURCE = {
    "Map": "patch-M.MPQ",
    "AreaTable": "patch-M.MPQ",
    "BattlemasterList": "patch-M.MPQ",
    "WorldSafeLocs": "patch-M.MPQ",
    "WorldMapArea": "patch-M.MPQ",
    "LoadingScreens": "patch-M.MPQ",
    "GameObjectDisplayInfo": "patch-M.MPQ",
    "Spell": "patch-T.MPQ",
    "SpellIcon": "patch-S.MPQ",
    "SpellVisual": "patch-S.MPQ",
    "SpellVisualKit": "patch-S.MPQ",
    "SpellVisualKitModelAttach": "patch-S.MPQ",
    "SpellVisualEffectName": "patch-S.MPQ",
}

LOC_MASK = 16712190      # enUS-only localised string block, as every row here uses
LOC_MASK_EMPTY = 16712188


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
        """A record with its string fields decoded, everything else raw uint32."""
        values = self.raw(rid)
        return [self.string(v) if f in string_fields else v for f, v in enumerate(values)]

    def blob(self):
        return struct.pack("<4sIIII", b"WDBC", self.count, self.fields, self.rec_size, len(self.strings)) \
            + bytes(self.records) + bytes(self.strings)


# ----------------------------------------------------------------- sources
_asc_cache = {}


def asc(name):
    if name not in _asc_cache:
        sys.path.insert(0, ASC_TOOLS)
        from mpq import MPQArchive  # noqa: E402
        archive = MPQArchive(os.path.join(ASC_DATA, ASC_SOURCE[name]))
        try:
            data = archive.read_file("DBFilesClient\\%s.dbc" % name)
        finally:
            archive.close()
        _asc_cache[name] = Dbc(data, name)
    return _asc_cache[name]


def asc_row(name, rid, **changes):
    row = asc(name).row(rid, set(SPECS[name][1]))
    for field, value in changes.items():
        row[int(field.lstrip("f"))] = value
    return row


def f32(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def u32(value):
    return value & 0xFFFFFFFF


def localized(text, mask=LOC_MASK):
    return [text] + [""] * 15 + [mask]


# ------------------------------------------------------------------- rows
MAP_ID = 998
AREA_IDS = (6051, 6060, 6061, 6136)
AREA_BITS = (3828, 3829, 3830, 3831)       # first free after 3827
WSG_ZONE_MUSIC = 243                       # what Warsong Gulch and Eye of the Storm play
LOADING_SCREEN = 334
WSL_FIRST = 52700
WSL_SOURCE = (5978, 5979, 6034, 6035, 6036, 6037)
SPELL_STACK = (92300, 92301, 92302, 92303)
SPELL_HOLDER = (92304, 92305, 92306, 92307)
ASC_STACK = (844536, 844538, 844540, 844542)
ASC_HOLDER = (844537, 844539, 844541, 844546)
AURA_INTERRUPT_ORB = 0x1A0000              # mount | change map | immune or lost selection
DURATION_INFINITE = 21
ORB_DISPLAY = 11533                        # murderball_pedestal_ball.m2
BEAM_DISPLAYS = (11960, 11961, 11962, 11963)
# The start gate: mogu_northgate_door_03 (Stand / Open / Opened), the door the
# MG_NORTHGATEB gatehouse is built around. Ascension's display 1013068, renumbered.
GATE_DISPLAY = 11964
ASC_GATE_DISPLAY = 1013068
BEAM_MODELS = ("spells\\auracyanverytall_v2.m2", "spells\\aurapurpleverytall_v2.m2",
               "spells\\auragreenverytall_v2.m2", "spells\\auraorangeverytall_v2.m2")
# Bounding box of the beam models, read out of the M2 (all four share it).
BEAM_BOX = (-0.92, -0.93, 0.37, 0.95, 0.94, 65.16)


def build_rows():
    rows = {name: [] for name in SPECS}

    # Map: Ascension's row, pointed at its own zone. No loading screen (owner,
    # 2026-10-15): LoadingScreenID 0 gives the client's default one. Row 334
    # below stays, unused, because the published archives already carry it.
    rows["Map"].append(asc_row("Map", MAP_ID, f22=AREA_IDS[0], f57=0))

    # AreaTable: Ascension's four rows. AreaBit renumbered (see header); the
    # ambience and music ids are Ascension's own and do not exist here, so the
    # zone plays the battleground music Warsong Gulch and Eye of the Storm use.
    for area, bit in zip(AREA_IDS, AREA_BITS):
        rows["AreaTable"].append(asc_row("AreaTable", area, f3=bit, f7=0, f8=WSG_ZONE_MUSIC, f9=0, f10=0))

    # BattlemasterList: Ascension's 790 as type 106, ten a side.
    rows["BattlemasterList"].append(asc_row("BattlemasterList", 790, f0=106, f28=10, f29=0, f30=10, f31=80))

    # PvpDifficulty: the same 10-69 brackets as Scarlet Chapel and the Violet Hold.
    for bracket, rid in enumerate((90998, 93239, 93240, 93241, 93242, 93243)):
        low = 10 + bracket * 10
        rows["PvpDifficulty"].append([rid, MAP_ID, bracket, low, low + 9, 0])

    # WorldSafeLocs: Ascension's starts and graveyards, renumbered in order.
    for i, src in enumerate(WSL_SOURCE):
        rows["WorldSafeLocs"].append(asc_row("WorldSafeLocs", src, f0=WSL_FIRST + i))

    # WorldStateUI: Ascension has none for this map (its score bar is Lua), so
    # these follow Eye of the Storm's two rows (166/167), the other 1600 race.
    for rid, icon, orbs, score in ((90028, "Interface\\TargetingFrame\\UI-PVP-Alliance", 9504, 9500),
                                   (90029, "Interface\\TargetingFrame\\UI-PVP-Horde", 9505, 9501)):
        row = [rid, MAP_ID, 0, 0, icon]
        row += localized("Orbs: %%%dw  Victory Points: %%%dw/%%9502w" % (orbs, score))
        row += localized("", LOC_MASK_EMPTY)
        row += [9503, 0, ""]
        row += localized("", LOC_MASK_EMPTY)
        row += ["", 0, 0, 0]
        rows["WorldStateUI"].append(row)

    # WorldMapArea: Ascension's 856, terrain-style art in TempleofKotmogu\.
    rows["WorldMapArea"].append(asc_row("WorldMapArea", 856, f0=9534))

    rows["LoadingScreens"].append(asc_row("LoadingScreens", LOADING_SCREEN))

    # Spells: the stacking debuff and the hidden aura that stacks it. Ascension
    # gave them a 30-minute duration; a match can outlast nothing that long, but
    # the battleground removes them itself, so they are simply permanent here.
    # Ascension's orange holder (844546) lacks the interrupt flags its three
    # siblings carry - without them mounting would not drop the orange orb.
    for stack, holder, a_stack, a_holder in zip(SPELL_STACK, SPELL_HOLDER, ASC_STACK, ASC_HOLDER):
        rows["Spell"].append(asc_row("Spell", a_stack, f0=stack, f40=DURATION_INFINITE))
        rows["Spell"].append(asc_row("Spell", a_holder, f0=holder, f40=DURATION_INFINITE,
                                     f32=AURA_INTERRUPT_ORB, f116=stack))

    for icon in (6182, 6183, 6184, 6185):
        rows["SpellIcon"].append(asc_row("SpellIcon", icon))
    for visual in (82278, 82279, 82280, 82281):
        rows["SpellVisual"].append(asc_row("SpellVisual", visual))
    for kit in (80003, 80004, 80005, 80006):
        rows["SpellVisualKit"].append(asc_row("SpellVisualKit", kit))
    for attach in range(80003, 80014):
        rows["SpellVisualKitModelAttach"].append(asc_row("SpellVisualKitModelAttach", attach))
    for effect in range(80024, 80029):
        rows["SpellVisualEffectName"].append(asc_row("SpellVisualEffectName", effect))

    rows["GameObjectDisplayInfo"].append(asc_row("GameObjectDisplayInfo", ORB_DISPLAY))
    # Its sound slots name Ascension's SoundEntries (28949...), which this client
    # does not have, so they are cleared.
    rows["GameObjectDisplayInfo"].append(asc_row("GameObjectDisplayInfo", ASC_GATE_DISPLAY, f0=GATE_DISPLAY,
                                                 **{"f%d" % f: 0 for f in range(2, 12)}))
    for display, model in zip(BEAM_DISPLAYS, BEAM_MODELS):
        rows["GameObjectDisplayInfo"].append(
            [display, model] + [0] * 10 + [f32(v) for v in BEAM_BOX] + [0])

    return rows


# ------------------------------------------------------------------ writer
def encode(dbc, name, row):
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
            out.append(u32(value))
    return out


def intern(dbc, text, pool):
    if text == "":
        return 0
    if text not in pool:
        pool[text] = len(dbc.strings)
        dbc.strings += text.encode("utf-8") + b"\0"
    return pool[text]


def upsert(path, name, rows, dry_run, rewrite=False):
    blob = open(path, "rb").read()
    dbc = Dbc(blob, name)
    nfields, strings = SPECS[name]
    if dbc.fields != nfields or dbc.rec_size != nfields * 4:
        raise ValueError("%s: file has %d fields / %d bytes, expected %d" % (path, dbc.fields, dbc.rec_size, nfields))

    pool = {}
    added = changed = same = 0
    for row in rows:
        values = encode(dbc, name, row)
        rid = values[0]
        if rid in dbc.index:
            current = dbc.row(rid, set(strings))
            if current == values:
                same += 1
                continue
            if not rewrite:
                # Every id here is meant to be new. A row that is already there
                # with other values is someone else's: stop rather than clobber.
                raise SystemExit("%s: row %d exists with different values - id collision? "
                                 "(--rewrite replaces it with ours)" % (path, rid))
            base = dbc.index[rid] * dbc.rec_size
            for f, value in enumerate(values):
                if current[f] == value:
                    continue
                raw = intern(dbc, value, pool) if f in strings else value
                struct.pack_into("<I", dbc.records, base + f * 4, raw)
            changed += 1
        else:
            packed = [intern(dbc, v, pool) if f in strings else v for f, v in enumerate(values)]
            dbc.records += struct.pack("<%dI" % nfields, *packed)
            dbc.index[rid] = dbc.count
            dbc.count += 1
            added += 1

    print("    %-28s +%d added, %d rewritten, %d already right" % (name + ".dbc", added, changed, same))
    if dry_run or not (added or changed):
        return
    backup = path + BACKUP_SUFFIX
    if not os.path.exists(backup):
        shutil.copy2(path, backup)
    with open(path, "wb") as fh:
        fh.write(dbc.blob())


def verify(path, name, rows):
    dbc = Dbc(open(path, "rb").read(), name)
    strings = set(SPECS[name][1])
    bad = 0
    for row in rows:
        values = encode(dbc, name, row)
        if values[0] not in dbc.index:
            print("    %s: row %d MISSING" % (name, values[0]))
            bad += 1
        elif dbc.row(values[0], strings) != values:
            print("    %s: row %d DIFFERS" % (name, values[0]))
            bad += 1
    print("    %-28s %s" % (name + ".dbc", "ok (%d rows)" % len(rows) if not bad else "%d bad" % bad))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=("show", "apply", "verify"))
    ap.add_argument("dir", nargs="?")
    ap.add_argument("--only", help="comma-separated DBC names")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--rewrite", action="store_true", help="replace rows whose id exists with other values")
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
            print("    %-28s not present here, skipped" % (name + ".dbc"))
            continue
        if args.command == "apply":
            upsert(path, name, rows[name], args.dry_run, args.rewrite)
        else:
            bad += verify(path, name, rows[name])
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
