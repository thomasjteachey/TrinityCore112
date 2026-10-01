"""Append the dungeon's named ability spells (92110-92151) to a Spell.dbc.

  python3 bba_ability_spells.py <dir holding Spell.dbc> [--dry-run]

Each ability is a clone of the stock spell the encounter script used in phase 3 (same visual,
targeting and effect shape), renamed to the design doc's ability and re-tuned so the tooltip
shows what actually lands:
  bp      {effect index: value}   stored as base = v-1, die = 1 (the client shows bp + die)
  effect  {effect index: type}    0 removes the effect (all of that slot's fields are zeroed)
  aura / misc / amp / radius      per effect index
  dur / cast                      SpellDuration / SpellCastTimes ids
  school / stack                  SchoolMask / StackAmount
Rank strings are cleared; Description and ToolTip get the ability text. Strings are appended,
never edited in place. Same backup / readback / no-existing-row-changes rules as the other tools.
"""
import os, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dbcrow import DBC

# SpellDuration ids: 21 = infinite, 39 = 2s, 28 = 5s, 31 = 8s, 29 = 12s
# SpellCastTimes ids: 1 = instant, 5 = 2s, 20 = 2.5s.  SpellRadius 14 = 8 yd.
ABILITIES = [
    # --- tribal trials ---
    (92110, 8599, 'Ritual Rhythm', 'The drums drive the champion on. Attack speed increased by 30%.',
     dict(dur=31, effect={0: 0}, bp={1: 30})),
    (92111, 8599, 'Blood Frenzy', 'Damage dealt increased by 25%.',
     dict(dur=21, effect={0: 6, 1: 0}, aura={0: 79}, misc={0: 127}, bp={0: 25})),
    (92112, 45052, 'Desperate Evocation', 'Regains mana over 8 sec. Cannot be interrupted.', {}),
    (92113, 16856, 'Sand Slash', 'Carves the target for 420 Physical damage.',
     dict(effect={0: 2, 1: 0}, bp={0: 420}, school=1)),
    (92114, 15581, 'Burrowing Ambush', 'Bursts out of the sand beneath a target for 650 Physical damage.',
     dict(effect={0: 2}, bp={0: 650}, school=1)),
    (92115, 21868, 'Sandstorm', 'Scouring sand deals 180 Nature damage to nearby enemies.',
     dict(effect={1: 0}, bp={0: 180})),
    (92116, 21060, 'Blinding Sand', 'Sand in the eyes. The target wanders blindly.', {}),
    (92117, 21868, 'Sand Trap', 'The dune collapses, dealing 300 Nature damage to enemies within 8 yards.',
     dict(effect={1: 0}, bp={0: 300}, radius={0: 14})),
    (92118, 15497, 'Iceblood Strike', 'Strikes with frozen blood for 450 Frost damage and slows movement.',
     dict(cast=1, bp={0: 450})),
    (92120, 19099, 'Frozen Ground', 'Deals 120 Frost damage every 2 sec to enemies standing on the ice.',
     dict(bp={0: 120})),
    (92121, 15497, 'Ice Spear', 'Hurls a spear of ice for 700 Frost damage.',
     dict(cast=20, bp={0: 700})),
    (92122, 16856, 'Thorned Strike', 'Rakes the target with thorns for 430 Physical damage.',
     dict(effect={0: 2, 1: 0}, bp={0: 430}, school=1)),
    (92123, 24840, 'Grasping Vines', 'Vines lash everything within 10 yards for 140 Nature damage every sec.',
     dict(bp={0: 140})),
    (92124, 12533, 'Venomous Breath', 'Breathes venom in a cone: 360 Nature damage and 40 more every 5 sec.',
     dict(bp={0: 40, 1: 360})),
    (92125, 30111, 'Spore Burst', 'Spores burst for 220 Nature damage to enemies within 10 yards.',
     dict(bp={0: 220})),
    # --- Zalvaxa ---
    (92126, 12739, 'Bone Spear', 'Hurls a spear of bone for 420 Shadow damage.', dict(bp={0: 420})),
    (92127, 15654, 'Grave Rot', '110 Shadow damage every 3 sec. When it ends, the dead rise.', dict(bp={0: 110})),
    (92128, 17820, 'Voodoo Hex', 'Healing received reduced by 75%.', {}),
    (92129, 1112, 'Corpse Explosion', 'The corpse bursts for 300 Shadow damage to enemies within 10 yards.',
     dict(cast=1, bp={0: 300})),
    (92130, 8599, 'Blood-Fed', 'Fed on the dead. Damage dealt increased by 10% per application.',
     dict(dur=21, effect={0: 6, 1: 0}, aura={0: 79}, misc={0: 127}, bp={0: 10}, stack=10)),
    (92131, 12380, 'Ritual of the Endless Dead', 'Raising the dead without end.', {}),
    # --- the twins ---
    (92132, 15654, 'Garrote', 'Garrotes the target: 100 Physical damage every 3 sec.',
     dict(bp={0: 100}, school=1)),
    (92133, 15581, 'Crimson Step', 'Steps out of the shadows for 650 Physical damage.',
     dict(effect={0: 2}, bp={0: 650}, school=1)),
    (92135, 8599, 'Blood Reunion', 'Within reach of her sister: damage dealt increased by 25% and 1% of maximum health restored every sec.',
     dict(dur=39, effect={0: 6, 1: 6}, aura={0: 79, 1: 20}, misc={0: 127, 1: 0}, bp={0: 25, 1: 1}, amp={1: 1000})),
    (92136, 12739, 'Shadowburn', 'Instantly blasts a weakened target for 900 Shadow damage.',
     dict(cast=1, bp={0: 900})),
    (92151, 8599, 'Blade Flurry', 'Attack speed increased by 30%.',
     dict(dur=29, effect={0: 0}, bp={1: 30})),
    # --- Vraka'ti ---
    (92138, 15232, 'Blood Bolt', 'Hurls a bolt of blood for 500 Shadow damage.', dict(bp={0: 500})),
    (92139, 15654, 'Sacrificial Mark', 'Marked for the cauldron. In 5 sec, blood erupts around you.',
     dict(dur=28, aura={0: 4}, bp={0: 0}, amp={0: 0})),
    (92140, 1112, 'Sacrificial Eruption', 'Blood erupts for 260 Shadow damage to everyone within 10 yards.',
     dict(cast=1, bp={0: 260})),
    (92141, 38051, 'Blood Chains', 'Bound in chains of blood. Cannot move.', {}),
    (92142, 17238, 'Blood Drain', 'Drains 180 health every sec to the caster.', dict(bp={0: 180})),
    (92143, 1112, 'Blood Nova', 'A wave of blood deals 350 Shadow damage to enemies within 10 yards.',
     dict(cast=5, bp={0: 350})),
    # --- Anok'Suten ---
    (92144, 28622, 'Web Cocoon', 'Wrapped in webbing. Cannot act, and takes 150 Nature damage every 2 sec. Destroy the cocoon to escape.',
     dict(bp={1: 150})),
    (92145, 60160, 'Shadow Web', 'Shadow webbing deals 150 Shadow damage every sec and slows those caught in it.',
     dict(bp={0: 150})),
    (92146, 15496, 'Piercing Legs', 'Impales the target and an adjacent enemy.', dict(bp={0: 300})),
    (92147, 17238, 'Blood Web', 'Drains 220 health every sec into Anok\'Suten. Move away to break the web.',
     dict(bp={0: 220})),
    (92148, 22883, 'Blood Harvest', 'Consumes the gathered blood to heal.', {}),
    (92149, 8599, 'The Last Thread', 'Damage dealt and attack speed increased by 30%.',
     dict(dur=21, effect={0: 6}, aura={0: 79}, misc={0: 127}, bp={0: 30, 1: 30})),
]

EFFECT_FIELDS = (71, 74, 77, 80, 83, 86, 89, 92, 95, 98, 104, 107, 110, 113, 116)


def build_overrides(sp, src_row, spec):
    ints, floats = {153: 0}, {}
    for e, typ in spec.get('effect', {}).items():
        if typ == 0:
            for base in EFFECT_FIELDS:
                ints[base + e] = 0
            for k in range(3):
                ints[122 + 3 * e + k] = 0
            floats[101 + e] = 0.0
            floats[119 + e] = 0.0
            floats[229 + e] = 0.0
            floats[216 + e] = 1.0
        else:
            ints[71 + e] = typ
            if sp.i32(src_row, 86 + e) == 0:
                ints[86 + e] = 1            # a new aura slot targets the caster
            floats[216 + e] = 1.0
    for e, v in spec.get('bp', {}).items():
        ints[80 + e] = v - 1
        ints[74 + e] = 1
        ints[77 + e] = 0                    # no per-level growth
    for e, v in spec.get('aura', {}).items():
        ints[95 + e] = v
    for e, v in spec.get('misc', {}).items():
        ints[110 + e] = v
    for e, v in spec.get('amp', {}).items():
        ints[98 + e] = v
    for e, v in spec.get('radius', {}).items():
        ints[92 + e] = v
    if 'dur' in spec:
        ints[40] = spec['dur']
    if 'cast' in spec:
        ints[28] = spec['cast']
    if 'school' in spec:
        ints[225] = spec['school']
    if 'stack' in spec:
        ints[49] = spec['stack']
    return ints, floats


def main():
    d = sys.argv[1]
    dry = '--dry-run' in sys.argv
    path = os.path.join(d, 'Spell.dbc')
    sp = DBC(path)
    assert sp.f == 234
    before = sp.n
    planned = []
    for sid, src, name, text, spec in ABILITIES:
        if sp.row(sid) is not None:
            print('%d already present - skipped' % sid)
            continue
        src_row = sp.row(src)
        assert src_row is not None, 'source %d missing' % src
        ints, floats = build_overrides(sp, src_row, spec)
        sp.append(src, sid, ints, floats, {136: name, 170: text, 187: text})
        planned.append((sid, name, spec))
        print('%d %-28s <- %d %s' % (sid, name, src, spec if spec else '(rename)'))
    if dry:
        print('dry run: rows %d -> %d' % (before, sp.n))
        return
    if not planned:
        return
    bak = path + '.bak-bba-abilities'
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
    sp.save(path)
    chk = DBC(path)
    for sid, name, spec in planned:
        r = chk.row(sid)
        assert r is not None and chk.s(chk.u(r, 136)) == name, sid
        for e, v in spec.get('bp', {}).items():
            assert chk.i32(r, 80 + e) + chk.i32(r, 74 + e) == v, (sid, e)
    old = DBC(bak)
    for a, b in zip(old.recs, chk.recs):
        assert a == b, 'an existing record changed'
    print('Spell.dbc rows %d -> %d, verified' % (old.n, chk.n))


if __name__ == '__main__':
    main()
