#!/usr/bin/env python3
"""Name the Robot Masters -- toolkit for the 27 PvP bot naming campaign.

Runs on the game box.  It needs the realm's worldserver.conf (for the character
database credentials, the same parse ~/wq.sh does) and ~/wow/bin/soapcmd.sh,
which renames a character live over loopback SOAP with no restart.

    roster                          the 27 bots: tier, spec, name, kills
    validate  < submissions.txt     check a pasted Discord submission dump
    poll W    < submissions.txt     poll bodies for wave W, one per bot
    apply     < winners.txt         rename the winners, credit the namers
    report                          kills gained since the last snapshot

Only `apply` writes anything, and only with --confirm.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

CONF = Path.home() / "wow/servers/tc-centurion/etc/worldserver.conf"
SOAPCMD = Path.home() / "wow/bin/soapcmd.sh"
REALM = "centurion"
SNAPSHOT = Path.home() / ".namebots-kills.json"

# Slot number -> the fixed facts about that bot.  Everything else (current name,
# race, kills, weapon) is read live, so a bot that has already been renamed
# reports under its new name.
WAVES = {1: "The Veterans", 2: "The Line", 3: "The Prototypes"}
BOTS = {
    # slot: (guid, wave, class, spec)
    1:  (100955, 1, "Warrior", "Arms"),
    2:  (100956, 1, "Paladin", "Holy"),
    3:  (100957, 1, "Hunter", "Marksmanship"),
    4:  (100958, 1, "Rogue", "Subtlety"),
    5:  (100960, 1, "Priest", "Discipline"),
    6:  (100959, 1, "Shaman", "Elemental"),
    7:  (100962, 1, "Mage", "Frost"),
    8:  (100963, 1, "Warlock", "Demonology"),
    9:  (100961, 1, "Druid", "Restoration"),
    10: (101165, 2, "Warrior", "Protection"),
    11: (101160, 2, "Paladin", "Retribution"),
    12: (101158, 2, "Hunter", "Survival"),
    13: (101159, 2, "Rogue", "Combat"),
    14: (101164, 2, "Priest", "Holy"),
    15: (101167, 2, "Shaman", "Restoration"),
    16: (101162, 2, "Mage", "Fire"),
    17: (101161, 2, "Warlock", "Affliction"),
    18: (101166, 2, "Druid", "Feral"),
    19: (101201, 3, "Warrior", "Fury"),
    20: (101198, 3, "Paladin", "Protection"),
    21: (101199, 3, "Hunter", "Beast Mastery"),
    22: (101193, 3, "Rogue", "Assassination"),
    23: (101196, 3, "Priest", "Shadow"),
    24: (101197, 3, "Shaman", "Enhancement"),
    25: (101202, 3, "Mage", "Arcane"),
    26: (101200, 3, "Warlock", "Destruction"),
    27: (101194, 3, "Druid", "Balance"),
}
GUID_SLOT = {guid: slot for slot, (guid, *_) in BOTS.items()}

RACES = {1: "Human", 2: "Orc", 3: "Dwarf", 4: "Night Elf",
         5: "Undead", 6: "Tauren", 7: "Gnome", 8: "Troll"}

# What the core will accept, from ObjectMgr::CheckPlayerName with StrictPlayerNames = 1:
# basic Latin only, no digits or spaces, 2-12 characters, and no letter three
# times in a row.  The profanity and reserved-name regexes live in the client's
# DBCs and are only enforced when the rename actually runs, so `apply` re-checks.
NAME_RE = re.compile(r"^[A-Za-z]{2,12}$")
# Deliberately loose about the name: a too-long or digit-bearing submission
# should come back as a stated rejection, not vanish as an unparsed line.
SUBMISSION_RE = re.compile(r"^\s*#?(\d{1,2})\s*[-:.)]?\s+(\S{1,30})\s*$")
DISCORD_STAMP_RE = re.compile(r"\s+—\s+(Today|Yesterday|\d{2}/\d{2}/\d{4})\b.*$")


# --------------------------------------------------------------------------- db

def _credentials():
    line = ""
    for raw in CONF.read_text(errors="replace").splitlines():
        if raw.startswith("CharacterDatabaseInfo"):
            parts = raw.split('"')
            if len(parts) > 1:
                line = parts[1]
            break
    fields = line.split(";")
    if len(fields) < 5:
        sys.exit(f"could not parse CharacterDatabaseInfo out of {CONF}")
    return fields[:5]


def query(sql):
    """Run SQL and return a list of dicts.  --batch keeps the output parsable."""
    host, port, user, password, db = _credentials()
    env = dict(os.environ, MYSQL_PWD=password)
    proc = subprocess.run(
        ["mysql", "-h", host, "-P", port, "-u", user, "-D", db, "--batch", "--raw"],
        input=sql, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        sys.exit(f"mysql failed: {proc.stderr.strip()}")
    lines = [ln for ln in proc.stdout.splitlines() if ln]
    if not lines:
        return []
    header = lines[0].split("\t")
    return [dict(zip(header, ln.split("\t"))) for ln in lines[1:]]


def execute(sql):
    host, port, user, password, db = _credentials()
    env = dict(os.environ, MYSQL_PWD=password)
    proc = subprocess.run(
        ["mysql", "-h", host, "-P", port, "-u", user, "-D", db],
        input=sql, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        sys.exit(f"mysql failed: {proc.stderr.strip()}")


def soap(command):
    proc = subprocess.run([str(SOAPCMD), REALM, command],
                          capture_output=True, text=True)
    return (proc.stdout + proc.stderr).strip()


# ------------------------------------------------------------------------ roster

def load_roster():
    guids = ",".join(str(guid) for guid, *_ in BOTS.values())
    rows = query(f"""
        SELECT c.guid, c.name, c.race, c.gender, c.totalKills,
               COALESCE(gm.pnote, '') AS namer,
               COALESCE(MAX(CASE WHEN ci.slot = 15 THEN it.name END), '') AS weapon
        FROM characters c
        LEFT JOIN guild_member gm ON gm.guid = c.guid
        LEFT JOIN character_inventory ci ON ci.guid = c.guid AND ci.bag = 0 AND ci.slot = 15
        LEFT JOIN item_instance ii ON ii.guid = ci.item
        LEFT JOIN centurionworld.item_template it ON it.entry = ii.itemEntry
        WHERE c.guid IN ({guids})
        GROUP BY c.guid, c.name, c.race, c.gender, c.totalKills, gm.pnote;
    """)
    roster = {}
    for row in rows:
        slot = GUID_SLOT[int(row["guid"])]
        guid, wave, klass, spec = BOTS[slot]
        roster[slot] = {
            "slot": slot, "guid": guid, "wave": wave, "class": klass, "spec": spec,
            "name": row["name"], "kills": int(row["totalKills"]), "namer": row["namer"],
            "race": RACES.get(int(row["race"]), "?"),
            "gender": "female" if row["gender"] == "1" else "male",
            "weapon": row["weapon"],
            "named": not row["name"].lower().startswith("bot"),
        }
    missing = sorted(set(BOTS) - set(roster))
    if missing:
        print(f"warning: no character row for slot(s) {missing}", file=sys.stderr)
    return roster


def taken_names():
    rows = query("SELECT LOWER(name) AS n FROM characters;")
    return {row["n"] for row in rows}


def cmd_roster(args):
    roster = load_roster()
    total = sum(bot["kills"] for bot in roster.values())
    for wave in sorted(WAVES):
        members = [bot for bot in roster.values() if bot["wave"] == wave]
        print(f"\n=== Wave {wave}: {WAVES[wave]} "
              f"({sum(b['kills'] for b in members):,} kills) ===")
        for bot in sorted(members, key=lambda b: b["slot"]):
            mark = "*" if bot["named"] else " "
            credit = f"  <- {bot['namer']}" if bot["namer"] else ""
            spec = f"{bot['class']} {bot['spec']}"
            print(f"{mark}{bot['slot']:>3}. {bot['name']:<13}{bot['race']:<10}"
                  f"{bot['gender']:<7}{spec:<22}"
                  f"{bot['kills']:>7,} kills  {bot['weapon']}{credit}")
    named = sum(1 for bot in roster.values() if bot["named"])
    print(f"\n{named}/{len(roster)} named, {total:,} honorable kills between them.")


# ---------------------------------------------------------------------- validate

def normalize(name):
    return name[:1].upper() + name[1:].lower()


def reject_reason(name, taken):
    """Why the core would refuse this name, or None if it looks good."""
    if not NAME_RE.match(name):
        if len(name) > 12:
            return "longer than 12 characters"
        if len(name) < 2:
            return "shorter than 2 characters"
        return "letters A-Z only (no digits, spaces or punctuation)"
    low = name.lower()
    for i in range(2, len(low)):
        if low[i] == low[i - 1] == low[i - 2]:
            return f"'{low[i]}' three times in a row"
    if low in taken:
        return "already a character on the realm"
    return None


def parse_submissions(text, slot=None):
    """Pull (slot, name, author) out of a pasted Discord dump.

    Two shapes, because two channel layouts are worth supporting.  Without
    --slot the dump is a shared channel and every line carries its bot:
    '7 Frostbite', '#7 Frostbite', '7: Frostbite'.  With --slot the dump is one
    bot's thread and a bare 'Frostbite' is the whole submission.

    Either way the author is the last line that looked like Discord's
    'name - Today at 8:14 PM' header.  It is a hint, and every command prints
    it next to the name so a wrong one is obvious before it becomes a credit.
    """
    author = "?"
    out = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if DISCORD_STAMP_RE.search(line):
            author = DISCORD_STAMP_RE.sub("", line).strip()[:40]
            continue
        match = SUBMISSION_RE.match(line)
        if match:
            out.append((int(match.group(1)), normalize(match.group(2)), author))
        elif slot is not None:
            out.append((slot, normalize(line.lstrip("#").strip()[:30]), author))
        elif len(line) <= 40:
            author = line
    return out


def collect(text, wave=None, thread_slot=None):
    """Group the dump per bot.  The same name from two people is not a mistake,
    it is agreement: it stays one entry and carries its count, which is how the
    list gets ranked when there are more than ten and the poll can only hold ten.
    """
    taken = taken_names()
    roster = load_roster()
    accepted, rejected = {}, []
    for order, (slot, name, author) in enumerate(parse_submissions(text, thread_slot)):
        if slot not in BOTS:
            rejected.append((slot, name, author, f"no bot #{slot}"))
            continue
        if wave and BOTS[slot][1] != wave:
            continue
        reason = reject_reason(name, taken)
        if reason:
            rejected.append((slot, name, author, reason))
            continue
        entries = accepted.setdefault(slot, {})
        entry = entries.get(name.lower())
        if entry:
            entry["count"] += 1
            if author not in entry["authors"]:
                entry["authors"].append(author)
        else:
            entries[name.lower()] = {"name": name, "authors": [author],
                                     "count": 1, "order": order}
    ranked = {slot: sorted(entries.values(), key=lambda e: (-e["count"], e["order"]))
              for slot, entries in accepted.items()}
    return roster, ranked, rejected


def cmd_validate(args):
    roster, accepted, rejected = collect(sys.stdin.read(), args.wave, args.slot)
    for slot in sorted(accepted):
        bot = roster.get(slot, {})
        print(f"\n#{slot} {bot.get('name', '?')} "
              f"({bot.get('class', '?')} {bot.get('spec', '')}) "
              f"- {len(accepted[slot])} name(s)")
        for entry in accepted[slot]:
            tally = f" x{entry['count']}" if entry["count"] > 1 else ""
            print(f"    {entry['name']:<13} {', '.join(entry['authors'])}{tally}")
    if rejected:
        print("\n--- rejected ---")
        for slot, name, author, reason in rejected:
            print(f"    #{slot} {name:<13} {author:<22} {reason}")
    total = sum(len(v) for v in accepted.values())
    print(f"\n{total} usable name(s) across {len(accepted)} bot(s), "
          f"{len(rejected)} rejected.")


def cmd_poll(args):
    """Discord poll bodies.  Ten answers is the platform cap, 55 chars each."""
    if not args.wave and not args.slot:
        sys.exit("poll needs a wave (1-3) or --slot N")
    roster, accepted, _ = collect(sys.stdin.read(), args.wave, args.slot)
    wanted = [args.slot] if args.slot else sorted(s for s in BOTS
                                                  if BOTS[s][1] == args.wave)
    for slot in wanted:
        bot = roster.get(slot)
        names = accepted.get(slot, [])
        if not bot:
            continue
        print(f"\n{'=' * 60}")
        print(f"POLL for #{slot} - {bot['name']} "
              f"({bot['race']} {bot['gender']} {bot['class']} {bot['spec']}, "
              f"{bot['kills']:,} kills)")
        print(f"Question: What do we call bot #{slot}? "
              f"({bot['class']} {bot['spec']}, {bot['kills']:,} kills)")
        if not names:
            print("  (no valid submissions)")
            continue
        for entry in names[:10]:
            answer = f"{entry['name']} - {entry['authors'][0]}"
            print(f"  {answer[:55]}")
        if len(names) > 10:
            cut = ", ".join(e["name"] for e in names[10:])
            print(f"  (cut, Discord allows 10 answers: {cut})")


# ------------------------------------------------------------------------- apply

def cmd_apply(args):
    """winners.txt: one per line, '<slot> <Name> <@handle>'.  Handle optional."""
    roster = load_roster()
    taken = taken_names()
    plan = []
    for raw in sys.stdin.read().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 2 or not parts[0].isdigit():
            sys.exit(f"cannot parse winner line: {raw!r}")
        slot, name = int(parts[0]), normalize(parts[1])
        handle = " ".join(parts[2:])[:31]
        if slot not in roster:
            sys.exit(f"no bot #{slot}")
        reason = reject_reason(name, taken)
        if reason:
            sys.exit(f"#{slot} {name}: {reason}")
        plan.append((slot, name, handle))

    for slot, name, handle in plan:
        bot = roster[slot]
        print(f"#{slot} {bot['name']} -> {name}"
              + (f"  (named by {handle})" if handle else ""))
    if not args.confirm:
        print(f"\n{len(plan)} rename(s) planned. Nothing changed - "
              f"re-run with --confirm to apply.")
        return

    for slot, name, handle in plan:
        bot = roster[slot]
        reply = soap(f".character rename {bot['name']} {name}")
        after = query(f"SELECT name FROM characters WHERE guid = {bot['guid']};")
        live = after[0]["name"] if after else "?"
        if live != name:
            print(f"#{slot} FAILED: still {live}. Server said: {reply}")
            continue
        if handle:
            escaped = handle.replace("\\", "\\\\").replace("'", "''")
            execute(f"UPDATE guild_member SET pnote = '{escaped}' "
                    f"WHERE guid = {bot['guid']};")
        print(f"#{slot} {bot['name']} is now {name}"
              + (f", credited to {handle}" if handle else ""))
    print("\nNames are live now. Guild notes show in-game after the next restart.")


# ------------------------------------------------------------------------ report

def cmd_report(args):
    roster = load_roster()
    now = {str(bot["slot"]): bot["kills"] for bot in roster.values()}
    before = {}
    if SNAPSHOT.exists():
        before = json.loads(SNAPSHOT.read_text()).get("kills", {})

    if not before:
        print("No previous snapshot; run with --snapshot to start the clock.")
    else:
        rows = []
        for bot in roster.values():
            delta = bot["kills"] - before.get(str(bot["slot"]), bot["kills"])
            rows.append((delta, bot))
        rows.sort(key=lambda r: -r[0])
        print("Kills since the last snapshot:\n")
        for rank, (delta, bot) in enumerate(rows, 1):
            credit = f" (named by {bot['namer']})" if bot["namer"] else ""
            print(f"{rank:>3}. {bot['name']:<13} +{delta:<6,} "
                  f"{bot['class']} {bot['spec']}{credit}")
        print(f"\nTotal: +{sum(d for d, _ in rows):,} honorable kills.")

    if args.snapshot:
        SNAPSHOT.write_text(json.dumps({"kills": now}, indent=1))
        print(f"\nSnapshot written to {SNAPSHOT}.")


# -------------------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("roster").set_defaults(func=cmd_roster)

    for name, func in (("validate", cmd_validate), ("poll", cmd_poll)):
        p = sub.add_parser(name)
        p.add_argument("wave", nargs="?", type=int, choices=sorted(WAVES))
        p.add_argument("--slot", type=int, choices=sorted(BOTS), metavar="N",
                       help="the dump is one bot's thread, so bare names count")
        p.set_defaults(func=func)

    p = sub.add_parser("apply")
    p.add_argument("--confirm", action="store_true",
                   help="actually rename; without it nothing is written")
    p.set_defaults(func=cmd_apply)

    p = sub.add_parser("report")
    p.add_argument("--snapshot", action="store_true",
                   help="write a new snapshot after printing")
    p.set_defaults(func=cmd_report)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
