"""Two SkillLineAbility repairs, in one pass over a binary SkillLineAbility.dbc.

(1) BLOOD FURY, TWICE. Row 11708 grants spell 20572 - the attack-power Blood
    Fury - with ClassMask 0. LearnSkillRewardedSpells skips the class test
    entirely when the mask is zero:

        if (ability->ClassMask && !(ability->ClassMask & classMask)) continue;

    so every orc gets it on top of their class variant: a warlock ends up with
    20572 AND 33702, a shaman with 20572 AND 33697. Warriors, hunters, rogues
    and death knights only ever saw one because 20572 IS their variant. Stock
    has ClassMask 45 for exactly this reason; B+ lost it. Restored to 45.

(2) THE WEAPON MASTER HANDS OUT 2H AXES AND MACES TO ANYONE. Spells 15985 and
    15987 - the trainer wrappers, distinct from the Enhancement talent 16269
    which teaches 197/199 itself - have NO SkillLineAbility rows at all, in B+
    or in stock. IsSpellFitByClassAndRace opens with

        if (bounds.first == bounds.second) return true;

    so with no rows there is no class gate whatsoever and the weapon master will
    teach them to anybody who can reach it. Two gate rows are added with
    AcquireMethod 0, which IsSpellFitByClassAndRace honours for the mask while
    LearnSkillRewardedSpells ignores entirely - so they gate the trainer and can
    never auto-grant anything.

        15985  2H Axes   skill 172  classmask 39    warrior|paladin|hunter|dk
        15987  2H Maces  skill 160  classmask 1059  warrior|paladin|dk|druid

    Shaman is excluded from both, leaving talent 16269 as their only route.

Everything is verified against current values before a byte is written, and the
result is read back and re-checked.

    python3 fix_sla.py <SkillLineAbility.dbc> [--apply]
"""
import struct, shutil, sys

F_ID, F_SKILL, F_SPELL, F_RACE, F_CLASS, F_MINRANK, F_ACQUIRE = 0, 1, 2, 3, 4, 7, 9

BLOOD_FURY = 20572
BF_EXPECT_CLASS = 0
BF_WANT_CLASS = 45

# skill, spell, racemask, classmask - ids are allocated above the file's max,
# because the realms do not share an id space (L+ already uses 22100/22101).
NEW_ROWS = [
    (172, 15985, 0, 39),
    (160, 15987, 0, 1059),
]

path = sys.argv[1]
apply = "--apply" in sys.argv

d = open(path, "rb").read()
magic, n, fields, rsize, sblock = struct.unpack_from("<4sIIII", d, 0)
assert magic == b"WDBC", magic
assert fields == 14, "unexpected field count %d" % fields

rows = []
for i in range(n):
    rows.append(list(struct.unpack_from("<%di" % fields, d, 20 + i * rsize)))

by_id = {r[F_ID]: r for r in rows}
by_spell = {}
for r in rows:
    by_spell.setdefault(r[F_SPELL], []).append(r)

ok = True

# Idempotent: a realm that is already correct is reported and skipped, not
# treated as a failure. B+ and L+ are not in the same state here.
bf = by_spell.get(BLOOD_FURY, [])
fix_bf = False
if len(bf) != 1:
    print("  Blood Fury: expected 1 row, found %d - refusing" % len(bf)); ok = False
elif bf[0][F_CLASS] == BF_WANT_CLASS:
    print("  Blood Fury row %d: ClassMask already %d - nothing to do" % (bf[0][F_ID], BF_WANT_CLASS))
elif bf[0][F_CLASS] != BF_EXPECT_CLASS:
    print("  Blood Fury row %d: ClassMask is %d, expected %d or %d - refusing"
          % (bf[0][F_ID], bf[0][F_CLASS], BF_EXPECT_CLASS, BF_WANT_CLASS)); ok = False
else:
    fix_bf = True
    print("  Blood Fury row %d: ClassMask %d -> %d" % (bf[0][F_ID], BF_EXPECT_CLASS, BF_WANT_CLASS))

next_id = max(by_id) + 1
planned = []
for skill, spell, race, cmask in NEW_ROWS:
    if by_spell.get(spell):
        print("  spell %d already gated by %d row(s) - nothing to do" % (spell, len(by_spell[spell])))
        continue
    planned.append((next_id, skill, spell, race, cmask))
    print("  add row id=%d skill=%d spell=%d classmask=%d acquire=0" % (next_id, skill, spell, cmask))
    next_id += 1

if not fix_bf and not planned:
    print("%s: already correct, nothing to do" % path.split("/")[-1])
    raise SystemExit(0)

if not ok:
    print("%s: preconditions failed, nothing written" % path.split("/")[-1])
    raise SystemExit(1)

if not apply:
    print("%s: %d rows, verified - rerun with --apply" % (path.split("/")[-1], n))
    raise SystemExit(0)

shutil.copy2(path, path + ".bak-slafix")

if fix_bf:
    bf[0][F_CLASS] = BF_WANT_CLASS
for rid, skill, spell, race, cmask in planned:
    row = [0] * fields
    row[F_ID], row[F_SKILL], row[F_SPELL] = rid, skill, spell
    row[F_RACE], row[F_CLASS] = race, cmask
    row[F_MINRANK], row[F_ACQUIRE] = 0, 0
    rows.append(row)

rows.sort(key=lambda r: r[F_ID])

out = bytearray()
out += struct.pack("<4sIIII", b"WDBC", len(rows), fields, rsize, sblock)
for r in rows:
    out += struct.pack("<%di" % fields, *r)
out += d[20 + n * rsize:]
open(path, "wb").write(bytes(out))

v = open(path, "rb").read()
vm, vn, vf, vr, vs = struct.unpack_from("<4sIIII", v, 0)
seen = {}
for i in range(vn):
    r = struct.unpack_from("<%di" % vf, v, 20 + i * vr)
    seen[r[F_ID]] = r
print("%s: rows %d -> %d" % (path.split("/")[-1], n, vn))
print("  verify Blood Fury classmask = %d" % seen[bf[0][F_ID]][F_CLASS])
for rid, skill, spell, race, cmask in planned:
    r = seen.get(rid)
    print("  verify id=%d spell=%d classmask=%d acquire=%d"
          % (rid, r[F_SPELL], r[F_CLASS], r[F_ACQUIRE]))
