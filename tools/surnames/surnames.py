#!/usr/bin/env python3
"""Family names for every character on the realm.

Runs on the game box, next to namebots.py: it needs the realm's
worldserver.conf (for the character database credentials, the same parse
~/wq.sh does) and ~/wow/bin/soapcmd.sh to tell the running realm to re-read
what it wrote.

    pools                    the surname pool for each race, with counts
    plan [--race R] [-n N]    what every character without a surname would get
    apply --confirm          write them, then `.reload character_surname`
    clear --confirm          blank every surname again (the undo)
    show <name>              one character's name, race and surname

Only `apply` and `clear` write anything, and only with --confirm.

The surname lives in `characters`.`surname` (see game/Miscellaneous/Surnames.h
for how it reaches the client); the first name is never touched, so nothing
here can break a login, a mail, a guild or anything else keyed on the name.
Which surname a character gets is decided by its guid, so a re-run is a no-op
and a character that was skipped gets the same answer next time.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

CONF = Path.home() / "wow/servers/tc-centurion/etc/worldserver.conf"
SOAPCMD = Path.home() / "wow/bin/soapcmd.sh"
REALM = "centurion"

RACES = {
    1: "Human", 2: "Orc", 3: "Dwarf", 4: "Night Elf", 5: "Undead",
    6: "Tauren", 7: "Gnome", 8: "Troll", 10: "Blood Elf", 11: "Draenei",
}

# ObjectMgr::CheckPlayerName, which a surname is held to as well: 2-12 letters,
# one alphabet, and never the same letter three times in a row.
NAME_RE = re.compile(r"^[A-Za-z]{2,12}$")

# Written to read like the race's own naming, and deliberately not the names of
# living lore figures - a hundred characters called Wrynn is not a family, it is
# a bug report. Clan and tribe names are fair game; those ARE surnames.
POOLS = {
    1: [  # Human
        "Ashcroft", "Ashford", "Barlow", "Bellamy", "Blackwood", "Brightmore",
        "Carrow", "Caulfield", "Cranwell", "Darrow", "Deveraux", "Dunwich",
        "Eastvale", "Fairbanks", "Fenwick", "Galloway", "Garrick", "Greyson",
        "Halstead", "Hartwell", "Havelock", "Holloway", "Kingsley", "Lockhart",
        "Marsden", "Merrick", "Norwood", "Oakhurst", "Pemberton", "Quinley",
        "Radcliffe", "Ravenholt", "Redpath", "Rothwell", "Sandover",
        "Shawcross", "Stanwick", "Stoneleigh", "Thornbury", "Vanbrook",
        "Wexford", "Whitlock", "Winslow", "Wycliffe",
    ],
    2: [  # Orc
        "Axebreaker", "Blackfang", "Bladefury", "Bloodaxe", "Bloodfist",
        "Bloodhowl", "Bonecrusher", "Darkmaul", "Deathfury", "Doomaxe",
        "Dreadfist", "Emberfist", "Feltusk", "Fellblade", "Gorefang",
        "Grimaxe", "Grimjaw", "Grimtusk", "Ironfist", "Ironjaw", "Ironmaul",
        "Rageblade", "Ragefist", "Redtusk", "Ruinhowl", "Savagehide",
        "Scarfang", "Screamblade", "Skullcleave", "Steelfang", "Stonefury",
        "Stormfang", "Thunderaxe", "Warfang", "Warhowl", "Wolfrider",
        "Wrathblade",
    ],
    3: [  # Dwarf
        "Anvilfist", "Anvilmar", "Barrelbelt", "Battlebrow", "Brewmantle",
        "Cinderforge", "Coalbraid", "Coldanvil", "Copperkeg", "Deepdelve",
        "Deepforge", "Direhammer", "Emberforge", "Flintbrow", "Forgewright",
        "Frostbeard", "Gemcutter", "Goldbraid", "Granitefist", "Grimbolt",
        "Hammerfall", "Hammerhold", "Ironbrow", "Ironkeg", "Ironshale",
        "Kegsmasher", "Longbeard", "Orebender", "Quarrystone", "Sootbeard",
        "Steelgrip", "Stonefist", "Stonehelm", "Stormbrew", "Thunderbrew",
        "Wildhammer", "Winterforge",
    ],
    4: [  # Night Elf
        "Ashenbough", "Bladeleaf", "Briarwind", "Dawnstrider", "Dewbright",
        "Duskwhisper", "Elunesong", "Feathermoon", "Fernbloom", "Frostglade",
        "Gladewalker", "Glaivewind", "Larkwing", "Leafwhisper", "Mistwood",
        "Moonbreeze", "Mooncaller", "Moonfall", "Moonshadow", "Nightbloom",
        "Nightbreeze", "Nightglade", "Nightsong", "Oakenmoon", "Owlsong",
        "Ravenwing", "Sablewind", "Shadecrest", "Silverbough", "Silverleaf",
        "Starbreeze", "Starcaller", "Starweaver", "Swiftwind", "Thistlebrook",
        "Thornbloom", "Wildbloom", "Willowbark", "Wolfsong",
    ],
    5: [  # Undead
        "Ashbone", "Ashenmourn", "Bittergrave", "Blackmire", "Bonecrypt",
        "Cinderbone", "Coldgrave", "Coldheart", "Corpsewood", "Darkmourn",
        "Deadmarsh", "Deathbough", "Dirgewood", "Dreadmourn", "Duskbane",
        "Gallowsworn", "Gloomvale", "Gravemire", "Graveswood", "Grimward",
        "Hollowbone", "Hollowgrave", "Marrowbane", "Mortlake", "Mournfall",
        "Nightgrave", "Pallidmoor", "Plaguewood", "Quietgrave", "Ravenmourn",
        "Rotheart", "Rotwood", "Sablecrypt", "Shadegrave", "Shroudveil",
        "Silentgrave", "Sorrowmoor", "Stitchbone", "Tombwood", "Wormwood",
    ],
    6: [  # Tauren
        "Bloodhoof", "Cloudmane", "Dawnhorn", "Deeproot", "Earthhoof",
        "Elderhoof", "Greatmane", "Grimtotem", "Heavyhoof", "Highmountain",
        "Longhorn", "Mistrunner", "Mossrunner", "Oakenhoof", "Plainstrider",
        "Proudhorn", "Rainchaser", "Ragetotem", "Redhoof", "Riverhorn",
        "Runetotem", "Sagehorn", "Silverhorn", "Skychaser", "Skyhorn",
        "Skyseer", "Snowhoof", "Spiritmane", "Stargrazer", "Stonehoof",
        "Stormhoof", "Stouthoof", "Sunwalker", "Swiftmane", "Tallgrass",
        "Thunderhoof", "Thunderhorn", "Whitehorn", "Wildhorn", "Wildmane",
        "Windmane", "Windtotem", "Wisemane",
    ],
    7: [  # Gnome
        "Bellowspark", "Boltwhistle", "Brasscog", "Coilspring", "Cogspark",
        "Copperbolt", "Cranktooth", "Dialspin", "Emberfuse", "Fizzlebang",
        "Fizzlespark", "Fusebox", "Gadgetspring", "Gearloose", "Gearspanner",
        "Gizmospark", "Gyrowrench", "Hexnut", "Ironspanner", "Knobtwist",
        "Nimblefinger", "Nutbolt", "Overcrank", "Pipewrench", "Quickfuse",
        "Ratchetcog", "Rivetspark", "Sparkfizzle", "Sprocketgear",
        "Steamvalve", "Tinkerpop", "Tinkerspan", "Twistbolt", "Voltcap",
        "Whirlygig", "Zapwhistle",
    ],
    8: [  # Troll
        "Amani", "Bloodscalp", "Bogstalker", "Bonecharm", "Bonerattle",
        "Darkspear", "Deathmask", "Drakkari", "Dreadmask", "Fetishclaw",
        "Frostmane", "Grimfeather", "Gurubashi", "Hakkari", "Headhunter",
        "Hexcaller", "Hexfang", "Jujuhand", "Lashtail", "Marshwalker",
        "Mojoheart", "Mossflayer", "Ragefang", "Sandfury", "Serpentcoil",
        "Shadowpine", "Spiritmask", "Swampfang", "Vilebranch", "Voodoofang",
        "Wildmask", "Witherbark",
    ],
    10: [  # Blood Elf
        "Bloodwrath", "Crimsonveil", "Dawnblade", "Dawnfire", "Dawnstar",
        "Duskblade", "Duskwither", "Emberfall", "Emberlight", "Eversong",
        "Felbane", "Firesong", "Goldenbough", "Manaveil", "Morningsong",
        "Phoenixfall", "Runeveil", "Sablewing", "Solarblade", "Sunbinder",
        "Sunfury", "Sunmender", "Sunsorrow", "Sunward", "Thornblade",
        "Truefire",
    ],
    11: [  # Draenei
        "Aetherwing", "Beaconlight", "Brightsoul", "Crystalsong",
        "Crystalvein", "Dawnbinder", "Dawnhammer", "Dawnspire", "Dawnward",
        "Everlight", "Faithbound", "Farseeker", "Glimmerward", "Holyward",
        "Hopebringer", "Keeplight", "Lightbearer", "Lightbinder",
        "Lightsworn", "Lightward", "Lucidsong", "Lumenveil", "Mercyhand",
        "Naarusworn", "Nobleheart", "Oathlight", "Prismwing", "Purelight",
        "Quietmind", "Radiantforge", "Runewatch", "Sablevault",
        "Sanctumward", "Shieldlight", "Soulmender", "Starforge", "Sunmantle",
        "Templeward", "Truelight", "Voidbane", "Warpstrider", "Wayfinder",
        "Wisdomlight",
    ],
}


def check_pools():
    """A pool entry the core would refuse is a bug in this file, not a surprise
    at 3am: every name is held to the core's own rules before anything runs."""
    problems = []
    for race, names in POOLS.items():
        seen = set()
        for name in names:
            if not NAME_RE.match(name):
                problems.append(f"{RACES[race]}: {name!r} is not 2-12 letters")
            lower = name.lower()
            for i in range(2, len(lower)):
                if lower[i] == lower[i - 1] == lower[i - 2]:
                    problems.append(f"{RACES[race]}: {name!r} has three of the same letter in a row")
                    break
            if lower in seen:
                problems.append(f"{RACES[race]}: {name!r} listed twice")
            seen.add(lower)
    if problems:
        sys.exit("pool problems:\n  " + "\n  ".join(problems))


def surname_for(guid, race):
    """Which name this character gets. Knuth's multiplicative hash, so
    neighbouring guids - a batch of bots created in one go - do not come out
    with neighbouring names."""
    pool = POOLS.get(int(race))
    if not pool:
        return None
    return pool[(int(guid) * 2654435761) % len(pool)]


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


def _mysql(sql, batch):
    host, port, user, password, db = _credentials()
    env = dict(os.environ, MYSQL_PWD=password)
    args = ["mysql", "-h", host, "-P", port, "-u", user, "-D", db]
    if batch:
        args += ["--batch", "--raw"]
    proc = subprocess.run(args, input=sql, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        sys.exit(f"mysql failed: {proc.stderr.strip()}")
    return proc.stdout


def query(sql):
    lines = [ln for ln in _mysql(sql, True).splitlines() if ln]
    if not lines:
        return []
    header = lines[0].split("\t")
    return [dict(zip(header, ln.split("\t"))) for ln in lines[1:]]


def execute(sql):
    _mysql(sql, False)


def soap(command):
    if not SOAPCMD.exists():
        return f"(no {SOAPCMD}; run `.reload character_surname` yourself)"
    proc = subprocess.run([str(SOAPCMD), REALM, command], capture_output=True, text=True)
    return (proc.stdout + proc.stderr).strip()


def column_exists():
    rows = query("SELECT COUNT(*) AS n FROM information_schema.COLUMNS "
                 "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'characters' "
                 "AND COLUMN_NAME = 'surname'")
    return bool(rows) and rows[0]["n"] != "0"


def load_characters(only_unnamed, race=None):
    where = ["(c.deleteDate IS NULL OR c.deleteDate = 0)"]
    if only_unnamed:
        where.append("c.surname = ''")
    if race is not None:
        where.append(f"c.race = {int(race)}")
    return query("SELECT c.guid, c.name, c.race, c.level, c.surname "
                 "FROM characters c WHERE " + " AND ".join(where) + " ORDER BY c.guid")


def quote(value):
    return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"


# ----------------------------------------------------------------------- commands

def cmd_pools(_args):
    total = 0
    for race in sorted(POOLS):
        names = POOLS[race]
        total += len(names)
        print(f"{RACES[race]:<10} {len(names):>3}  {', '.join(names[:6])} ...")
    print(f"\n{total} names over {len(POOLS)} races")


def cmd_plan(args):
    rows = load_characters(only_unnamed=not args.all, race=args.race)
    if not rows:
        print("nothing to do")
        return

    per_race = {}
    shown = 0
    for row in rows:
        surname = surname_for(row["guid"], row["race"])
        if surname is None:
            print(f"!! {row['name']} (guid {row['guid']}) has race {row['race']}, which has no pool")
            continue
        per_race[row["race"]] = per_race.get(row["race"], 0) + 1
        if shown < args.limit:
            print(f"  {row['name']} {surname}   ({RACES.get(int(row['race']), row['race'])}, level {row['level']}, guid {row['guid']})")
            shown += 1

    if shown < len(rows):
        print(f"  ... and {len(rows) - shown} more")
    print()
    for race in sorted(per_race, key=lambda r: -per_race[r]):
        print(f"  {RACES.get(int(race), race):<10} {per_race[race]:>5}")
    print(f"\n{sum(per_race.values())} character(s) would be named")


def cmd_apply(args):
    rows = load_characters(only_unnamed=not args.all, race=args.race)
    if not rows:
        print("every character already has a surname")
        return

    pairs = []
    for row in rows:
        surname = surname_for(row["guid"], row["race"])
        if surname is not None:
            pairs.append((int(row["guid"]), surname))

    if not args.confirm:
        print(f"{len(pairs)} character(s) would be named; re-run with --confirm")
        return

    written = 0
    for start in range(0, len(pairs), 200):
        batch = pairs[start:start + 200]
        cases = " ".join(f"WHEN {guid} THEN {quote(surname)}" for guid, surname in batch)
        guids = ",".join(str(guid) for guid, _ in batch)
        execute(f"UPDATE `characters` SET `surname` = CASE `guid` {cases} END WHERE `guid` IN ({guids});")
        written += len(batch)
        print(f"  {written}/{len(pairs)}")

    print(soap(".reload character_surname"))
    print(f"{written} character(s) named")


def cmd_clear(args):
    if not args.confirm:
        rows = query("SELECT COUNT(*) AS n FROM characters WHERE surname <> ''")
        print(f"{rows[0]['n']} surname(s) would be removed; re-run with --confirm")
        return
    execute("UPDATE `characters` SET `surname` = '' WHERE `surname` <> '';")
    print(soap(".reload character_surname"))
    print("every surname removed")


def cmd_show(args):
    rows = query(f"SELECT guid, name, race, level, surname FROM characters WHERE name = {quote(args.name)}")
    if not rows:
        sys.exit(f"no character called {args.name}")
    for row in rows:
        race = RACES.get(int(row["race"]), row["race"])
        current = row["surname"] or "(none)"
        would = surname_for(row["guid"], row["race"])
        print(f"{row['name']} {current}   ({race}, level {row['level']}, guid {row['guid']})")
        print(f"  unnamed, this character would get: {would}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("pools").set_defaults(func=cmd_pools)

    for name, func in (("plan", cmd_plan), ("apply", cmd_apply)):
        p = sub.add_parser(name)
        p.add_argument("--race", type=int, help="only this race id")
        p.add_argument("--all", action="store_true", help="include characters that already have a surname (replaces it)")
        p.set_defaults(func=func)
    sub.choices["plan"].add_argument("-n", "--limit", type=int, default=25, help="how many to print")
    sub.choices["apply"].add_argument("--confirm", action="store_true")

    p = sub.add_parser("clear")
    p.add_argument("--confirm", action="store_true")
    p.set_defaults(func=cmd_clear)

    p = sub.add_parser("show")
    p.add_argument("name")
    p.set_defaults(func=cmd_show)

    args = parser.parse_args()
    check_pools()
    if args.command != "pools" and not column_exists():
        sys.exit("`characters` has no `surname` column - apply the ALTER TABLE in README.md first")
    args.func(args)


if __name__ == "__main__":
    main()
