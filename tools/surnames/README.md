# Surnames

Family names for every character: `Elgrom Doomhammer` instead of `Elgrom`.

The first name is never touched. It stays what it always was, stays unique, and
stays the key everything else is looked up by - the surname is a second column
that is pasted on when a name goes out to the client
(`src/server/game/Miscellaneous/Surnames.h`). That is what makes this safe to
run over a live roster: the worst case is a column full of words nobody reads.

## What has to exist first

**1. The column** (by hand, on each realm's character database - the core probes
for it and stays off without it):

```sql
ALTER TABLE `characters` ADD COLUMN `surname` VARCHAR(12) NOT NULL DEFAULT '' AFTER `name`;
```

Check `createCopyOfChar` first: if that stored procedure copies characters with
`SELECT *` into a fixed column list, add the column at the END of the table
instead (leave off `AFTER name`), or character creation breaks.

**2. The config key**, in `worldserver.conf`:

```
Centurion.Surnames.Enable = 1
```

`.reload config` is enough for the key; the column is only probed at startup and
by `.reload character_surname`.

## Naming the realm

Deploy the script (it has to run on the game box - the database and SOAP are
both local to it):

```bash
cat tools/surnames/surnames.py | plink -agent -batch brokilodeluxe@192.168.1.226 "cat > ~/wow/bin/surnames.py && chmod +x ~/wow/bin/surnames.py"
```

Then:

```bash
surnames.py pools              # the pool per race, and a check that every name is legal
surnames.py plan               # what each character would be called; writes nothing
surnames.py apply              # says how many; still writes nothing
surnames.py apply --confirm    # writes them, then `.reload character_surname`
```

`apply` only fills characters that have no surname yet, so it is safe to re-run
as new characters (and new bots) appear. `--all` replaces existing ones,
`--race N` limits it to one race, and `clear --confirm` empties the column
again - which is the whole undo, since nothing else was changed.

Which name a character gets is decided by its guid, so the answer is stable: a
re-run gives everyone the same surname they had, and a character skipped today
gets the same one tomorrow.

## The pools

377 names over the ten playable races, written to read like the race's own
naming and deliberately avoiding living lore figures - a hundred characters
called Wrynn is not a family, it is a bug report. Clan and tribe names (Amani,
Wildhammer, Darkspear) are fair game; those ARE surnames.

Every entry is checked against the core's own rules before anything runs: 2-12
letters, one alphabet, and never the same letter three times in a row
(`ObjectMgr::CheckPlayerName`). A name that would be refused is a bug in the
pool, so `check_pools()` fails the whole run rather than skipping it.

Duplicates are fine and left alone. Two players called Bloodhoof are two
players called Bloodhoof; only the first name has to be unique.

## Afterwards

- Players pick their own on the character create screen from then on
  (`clientedits/framexml/staged-centurion-ui`, the LAST NAME box).
- `.character surname <name> <surname>` changes one, `.character surname <name>
  none` takes it away, and `.character surname <name>` reports it.
- A bulk edit made straight in SQL reaches a running realm with
  `.reload character_surname`.
