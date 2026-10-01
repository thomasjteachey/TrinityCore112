"""Append Blightblood's eight boon auras (92100-92107) to a Spell.dbc.

  python3 bba_spells.py <dir holding Spell.dbc> [--dry-run]

Each boon is cloned from Enrage 8599 (a generic instant self-buff) and then fully rewritten:
permanent (SpellDuration -1), instant, self, no cost, not dispellable, cannot be cancelled
(SPELL_ATTR0_CANT_CANCEL) and survives death (SPELL_ATTR3_DEATH_PERSISTENT) - the instance
script removes them when a player leaves the dungeon and re-applies earned ones on entry.
All three effect slots are cleared before the boon's own effects are written. Icons are
borrowed from stock spells. Values are stored base = v-1, die = 1 (the client shows bp+die).

Speed boons use SPELL_AURA_MOD_SPEED_ALWAYS (129), which multiplies, so the two of them stack
with each other and with mounts; MOD_INCREASE_SPEED (31) only takes the single highest.
Same backup/readback rules as bba_dbc.py: never overwrite an id, back up once, verify.
"""
import os, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dbcrow import DBC

TEMPLATE = 8599
CANT_CANCEL = 0x80000000
DEATH_PERSISTENT = 0x00100000

# id, name, icon donor spell, [(aura, value, misc)], text
BOONS = [
    (92100, 'Boon of the Dune', 2983, [(129, 2, 0)],
     'Movement speed increased by 2%.'),
    (92101, 'Boon of the Buried Fang', 8676, [(79, 2, 1)],
     'Physical damage dealt increased by 2%.'),
    (92102, 'Boon of the Frozen Breath', 168, [(290, 2, 0)],
     'Critical strike chance with all attacks and spells increased by 2%.'),
    (92103, 'Boon of the Frostbound', 11426, [(87, -2, 127)],
     'Damage taken reduced by 2%.'),
    (92104, 'Boon of the Living Root', 22812, [(133, 2, 0)],
     'Maximum health increased by 2%.'),
    (92105, 'Boon of the Thorned Soul', 467, [(79, 2, 127)],
     'All damage dealt increased by 2%.'),
    (92106, 'Boon of Defiance', 20572, [(79, 3, 127), (136, 3, 0)],
     'All damage and healing done increased by 3%.'),
    (92107, 'Boon of the Unbound', 20589, [(129, 3, 0), (117, 3, 12), (117, 3, 5)],
     'Movement speed increased by 3%, and 3% chance to resist stun and fear effects.'),
]


def main():
    d = sys.argv[1]
    dry = '--dry-run' in sys.argv
    path = os.path.join(d, 'Spell.dbc')
    dur = DBC(os.path.join(d, 'SpellDuration.dbc')) if os.path.exists(os.path.join(d, 'SpellDuration.dbc')) else None
    inf = 21
    if dur:
        cands = [dur.u(r, 0) for r in dur.recs if dur.i32(r, 1) == -1]
        assert 21 in cands, 'SpellDuration 21 is not -1 here: %s' % cands
    sp = DBC(path)
    assert sp.f == 234, 'Spell.dbc has %d fields' % sp.f
    before = sp.n
    for sid, name, icon_from, effs, text in BOONS:
        if sp.row(sid) is not None:
            print('%d already present - skipped' % sid)
            continue
        donor = sp.row(icon_from)
        icon = sp.u(donor, 133) if donor else 1
        ints = {1: 0, 2: 0, 3: 0, 4: CANT_CANCEL, 5: 0, 6: 0, 7: DEATH_PERSISTENT, 8: 0, 9: 0, 10: 0, 11: 0,
                28: 1, 29: 0, 30: 0, 31: 0, 32: 0, 33: 0, 34: 0, 35: 0, 36: 0, 37: 0, 38: 0, 39: 0,
                40: inf, 41: 0, 42: 0, 43: 0, 44: 0, 45: 0, 46: 1, 49: 0,
                131: 0, 132: 0, 133: icon, 134: 0, 135: 0, 153: 0,
                204: 0, 205: 0, 206: 0, 208: 0, 209: 0, 210: 0, 211: 0, 212: 0, 214: 0, 225: 1}
        floats = {}
        for e in range(3):
            for base in (71, 74, 77, 80, 83, 86, 89, 92, 95, 98, 104, 107, 110, 113, 116):
                ints[base + e] = 0
            for k in range(3):
                ints[122 + 3 * e + k] = 0
            floats[101 + e] = 0.0
            floats[119 + e] = 0.0
            floats[216 + e] = 1.0
            floats[229 + e] = 0.0
        for e, (aura, value, misc) in enumerate(effs):
            ints[71 + e] = 6            # SPELL_EFFECT_APPLY_AURA
            ints[86 + e] = 1            # TARGET_UNIT_CASTER
            ints[95 + e] = aura
            ints[80 + e] = value - 1
            ints[74 + e] = 1
            ints[110 + e] = misc
        sp.append(TEMPLATE, sid, ints, floats, {136: name, 170: text, 187: text})
        print('%d %-26s icon %d effects %s' % (sid, name, icon, effs))
    if dry:
        print('dry run: rows %d -> %d' % (before, sp.n))
        return
    bak = path + '.bak-bba'
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
    sp.save(path)
    chk = DBC(path)
    for sid, name, _, effs, text in BOONS:
        r = chk.row(sid)
        assert r is not None and chk.s(chk.u(r, 136)) == name and chk.s(chk.u(r, 170)) == text
        for e, (aura, value, misc) in enumerate(effs):
            assert chk.i32(r, 95 + e) == aura and chk.i32(r, 80 + e) + chk.i32(r, 74 + e) == value
    old = DBC(bak)
    for a, b in zip(old.recs, chk.recs):
        assert a == b, 'an existing record changed'
    print('Spell.dbc rows %d -> %d, verified' % (old.n, chk.n))


if __name__ == '__main__':
    main()
