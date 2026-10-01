"""Blood Beneath the Arena - append the instance's rows to a set of binary DBCs.

  python3 bba_dbc.py <dbc dir> [--dry-run]

Every row is cloned from a stock template row OF THE SAME FILE (so each archive / data dir
keeps its own lineage), then the listed fields are overridden. Strings are appended to the
string block; no existing record or string moves. Refuses if a target id already exists,
if a template is missing, or if the field count differs from the expected layout. Writes
<name>.bak-bba once (never overwrites an existing backup), then reads every file back.

Map 1685 is a 5-man dungeon cloned from Zul'Farrak (209) whose Directory is a private copy
of the one Stranglethorn ADT that holds the cave (BloodBeneathArena_31_56.adt). Its own
zone is AreaTable 30609 (cloned from Zul'Farrak's 1176, Zul'Gurub's ambience/music),
AreaBit 3827 (stock and custom max is 3826, no duplicates). MapDifficulty 759 gives it a
normal-difficulty 5-player row. Item 204100 is the quest reward Treasure Box (Item.dbc
row so the client shows its icon and lets it be opened).
"""
import os, shutil, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dbcrow import DBC

MAP_ID = 1685
AREA_ID = 30609
AREA_BIT = 3827
MAPDIFF_ID = 759
ITEM_ID = 204100

# Map.dbc corpse/entrance point = the Tunnel Watcher outside the arena (map 0).
ENTRANCE_X, ENTRANCE_Y = -13277.4, 127.372

PLAN = [
    # file, fields, template, new id, ints, floats, strings
    ('Map.dbc', 66, 209, MAP_ID,
     {2: 1, 3: 0, 4: 0, 22: 0, 57: 161, 59: 0, 63: 0, 64: 0, 65: 5},
     {60: ENTRANCE_X, 61: ENTRANCE_Y},
     {1: 'BloodBeneathArena', 5: 'Blood Beneath the Arena'}),
    ('AreaTable.dbc', 36, 1176, AREA_ID,
     {1: MAP_ID, 2: 0, 3: AREA_BIT, 4: 0, 5: 0, 6: 11, 7: 32, 8: 3, 10: 60},
     {},
     {11: 'Blood Beneath the Arena'}),
    ('MapDifficulty.dbc', 23, 23, MAPDIFF_ID,
     {1: MAP_ID, 2: 0, 20: 0, 21: 5},
     {}, {}),
    ('Item.dbc', 8, 20602, ITEM_ID,
     {}, {}, {}),
]


def main():
    d = sys.argv[1]
    dry = '--dry-run' in sys.argv
    only = [a for a in sys.argv[2:] if a.endswith('.dbc')]
    for name, nf, tmpl, nid, ints, floats, strings in PLAN:
        if only and name not in only:
            continue
        path = os.path.join(d, name)
        if not os.path.exists(path):
            print('%-18s absent in %s - skipped' % (name, d))
            continue
        db = DBC(path)
        if db.f != nf:
            raise SystemExit('%s: %d fields, expected %d - refusing' % (path, db.f, nf))
        if db.row(nid) is not None:
            print('%-18s id %d already present - skipped' % (name, nid))
            continue
        if name == 'AreaTable.dbc':
            bits = [db.u(r, 3) for r in db.recs]
            if AREA_BIT in bits:
                raise SystemExit('%s: AreaBit %d already used' % (path, AREA_BIT))
        before = db.n
        db.append(tmpl, nid, ints, floats, strings)
        if dry:
            print('%-18s would append %d (from %d): rows %d -> %d' % (name, nid, tmpl, before, db.n))
            continue
        bak = path + '.bak-bba'
        if not os.path.exists(bak):
            shutil.copy2(path, bak)
        db.save(path)
        # read back
        chk = DBC(path)
        r = chk.row(nid)
        assert chk.n == before + 1, 'row count'
        for i, v in ints.items():
            assert chk.i32(r, i) == v, (name, i, chk.i32(r, i), v)
        for i, v in floats.items():
            assert abs(chk.fl(r, i) - v) < 1e-3, (name, i)
        for i, v in strings.items():
            assert chk.s(chk.u(r, i)) == v, (name, i)
        # every pre-existing row byte-identical
        old = DBC(bak)
        for a, b in zip(old.recs, chk.recs):
            assert a == b, 'an existing record changed'
        print('%-18s appended %d (from %d): rows %d -> %d, verified' % (name, nid, tmpl, before, chk.n))


if __name__ == '__main__':
    main()
