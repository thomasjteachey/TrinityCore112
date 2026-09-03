"""Beast Rider is earned at 40, not handed to a level 1 hunter.

SkillLineAbility row 22020 grants spell 89799 on skill line 50 (Beast Mastery,
the hunter class skill) with AcquireMethod 2 - LEARNED_ON_SKILL_LEARN - which
fires the moment the skill line is learned. For a class skill that is character
creation, so every hunter had the mount from level 1.

AcquireMethod 1 is LEARNED_ON_SKILL_VALUE, and it is the only mode that honours
MinSkillLineRank: Player::LearnSkillRewardedSpells gates on

    if (skillValue < ability->MinSkillLineRank &&
        ability->AcquireMethod == SKILL_LINE_ABILITY_LEARNED_ON_SKILL_VALUE)
        RemoveSpell(ability->Spell);

Class skill rank tracks level at five per level, so MinSkillLineRank 200 is
level 40 - which is when mounts arrive anyway. It is also self-cleaning: the
same branch REMOVES the spell from anyone below the rank, so the hunters who
already have it early lose it on their next skill update without a SQL sweep.

SERVER ONLY. The client ships its own, different SkillLineAbility.dbc and the
two are deliberately not the same file; copying between them is what broke
.gm diagnostics before. Nothing here needs the client - the server decides what
a character knows and sends it in SMSG_INITIAL_SPELLS.

    python3 gate_beast_rider.py <SkillLineAbility.dbc> [--apply]
"""
import struct, shutil, sys

SPELL = 89799
F_SPELL, F_MINRANK, F_ACQUIRE, F_CLASSMASK = 2, 7, 9, 4

EXPECT = {F_SPELL: SPELL, F_MINRANK: 1, F_ACQUIRE: 2, F_CLASSMASK: 4}
WRITES = {F_MINRANK: 200, F_ACQUIRE: 1}

path = sys.argv[1]
apply = "--apply" in sys.argv

d = bytearray(open(path, "rb").read())
magic, n, fields, rsize, sblock = struct.unpack_from("<4sIIII", d, 0)
assert magic == b"WDBC", magic

hits = []
for i in range(n):
    off = 20 + i * rsize
    if struct.unpack_from("<i", d, off + F_SPELL * 4)[0] == SPELL:
        hits.append(off)

if len(hits) != 1:
    print("%s: expected exactly one row for %d, found %d - nothing written"
          % (path.split("/")[-1], SPELL, len(hits)))
    raise SystemExit(1)

off = hits[0]
ok = True
for field, want in EXPECT.items():
    got = struct.unpack_from("<i", d, off + field * 4)[0]
    if got != want:
        ok = False
        print("  field %-2d = %-8d expect %-8d MISMATCH" % (field, got, want))
if not ok:
    print("%s: row does not look as expected - nothing written" % path.split("/")[-1])
    raise SystemExit(1)
print("  row verified (spell=%d classmask=4 acquire=2 minrank=1)" % SPELL)

if not apply:
    print("%s: verified, --apply not given" % path.split("/")[-1])
    raise SystemExit(0)

shutil.copy2(path, path + ".bak-beastrider")
for field, value in WRITES.items():
    struct.pack_into("<i", d, off + field * 4, value)
open(path, "wb").write(bytes(d))

old = open(path + ".bak-beastrider", "rb").read()
diffs = [i for i in range(len(old)) if old[i] != d[i]]
expected = set()
for field in WRITES:
    expected |= set(range(off + field * 4, off + field * 4 + 4))
print("  AcquireMethod 2 -> 1, MinSkillLineRank 1 -> 200 (level 40)")
print("%s: %d bytes changed, all intended: %s"
      % (path.split("/")[-1], len(diffs), set(diffs).issubset(expected)))
