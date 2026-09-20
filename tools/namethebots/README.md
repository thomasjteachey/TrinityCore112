# Name the Robot Masters

A three-week Discord campaign that lets the community name the 27 PvP bots, and
the tooling to run it. `discord.md` holds every post to paste; `namebots.py`
does the parts a human should not do by hand.

## The 27 bots

All on account 79 in `centurioncharacters`, all level 60, all in
&lt;The Robot Masters&gt;, one per class/spec, nine per difficulty tier. The
campaign numbers them 1–27 and releases them a tier at a time:

| Wave | Tier | Bots | Kills each | Gear |
|---|---|---|---|---|
| 1 — The Veterans | easy | 1–9 | ~40,000 | blues |
| 2 — The Line | medium | 10–18 | ~11,000 | epics |
| 3 — The Prototypes | hard | 19–27 | ~2,000 | full epic sets |

The tier is `Playerbot.BgFill.Tier.*` in `etc/playerbots.conf`, which selects
bots **by guid**, so renaming them changes nothing about how they queue or
fight.

## Weekly loop

Deploy the script once (it has to run on the game box — SOAP is loopback-only):

```bash
cat tools/namethebots/namebots.py | plink -agent -batch brokilodeluxe@192.168.1.226 "cat > ~/wow/bin/namebots.py && chmod +x ~/wow/bin/namebots.py"
```

**Monday** — paste the wave post and its nine cards from `discord.md`, start a
thread on each card.

**Thursday night** — copy each thread's replies into a file and check them:

```bash
namebots.py validate --slot 1 < thread1.txt
```

Rejects come back with the reason, in the wording the rules post uses. A name
two people submitted independently collapses into one entry and ranks first.

**Friday** — build the polls. Ten answers is Discord's cap, so the list is cut
at ten, least-agreed first:

```bash
namebots.py poll 1 < all-threads.txt
```

**Sunday night** — write the winners as `<slot> <Name> <@handle>`, one per
line, and apply them:

```bash
namebots.py apply < winners.txt              # prints the plan, changes nothing
namebots.py apply --confirm < winners.txt    # renames and credits
```

`apply` re-checks every name, renames over SOAP, confirms the new name came
back from the database, and only then writes the namer into the bot's guild
note. A rename that the core refuses is reported and skipped; the rest still go
through.

**Any time after** — the ongoing hook:

```bash
namebots.py report --snapshot
```

Kills gained per bot since the last snapshot, sorted, with each namer credited.
Paste it weekly.

## What a rename actually does

- `.character rename <old> <new>` works on an offline character, writes the
  database **and** `CharacterCache`, and needs no restart
  ([cs_character.cpp:286](../../src/server/scripts/Commands/cs_character.cpp:286)).
  Names are live within seconds.
- Battleground clones display the source bot's name
  ([PlayerbotObcClone.cpp:2045](../../src/server/scripts/Playerbot/Pvp/PlayerbotObcClone.cpp:2045)),
  so the new name shows up in the next match that fills.
- The core enforces 2–12 basic-Latin letters, no digits or spaces, and no
  letter three times in a row (`StrictPlayerNames = 1` on this realm), plus the
  profanity and reserved-name regexes from the client DBCs. `namebots.py`
  checks everything except the DBC regexes locally, and catches those at apply
  time by verifying the name actually changed.

## Two things to know

- **Guild notes appear in-game after the next realm restart.** `pnote` is
  `varchar(31)`, and the guild is held in memory, so the database write is
  permanent but the in-game roster picks it up on the next load. The names
  themselves are immediate; only the credit line lags.
- **Two debug-log gates identify bots by the `Bot` name prefix**
  ([Unit.cpp:12121](../../src/server/game/Entities/Unit/Unit.cpp:12121),
  [MotionMaster.cpp:67](../../src/server/game/Movement/MotionMaster.cpp:67)).
  They only gate `TC_LOG_DEBUG` on `playerbots.pvp.motion`, which is off at the
  current log level, so nothing breaks — but once the bots are renamed that
  trace will never fire again. If it is ever needed, key it on account 79
  instead of the name.
