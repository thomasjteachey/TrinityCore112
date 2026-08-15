# Repository Agent Instructions

- Do **not** implement gameplay/code changes under `playerbot reference/`; that directory is reference-only.
- If playerbot behavior changes are requested, apply them in the active/source code location used by this repository build, not in the reference mirror.
- Do **not** add new per-battleground gameplay or movement special cases; prefer generic playerbot pathing fixes that work across battlegrounds.
- Do **not** run builds unless the user explicitly asks for one.

---

# Deployment reference

Verified 2026-08-12 by direct inspection. Claims that are inferred
rather than checked say so. If something here contradicts what you
observe on the server, trust the server and fix this file.

## 1. Connections

### SSH (Linux host `tyrantserver`, 192.168.1.226)

```bash
MSYS_NO_PATHCONV=1 plink -batch \
  -hostkey "SHA256:A+/c0SFXvxUzvp9UtQeh/49loAJqkby0eEXNVy0O9Wc" \
  brokilodeluxe@192.168.1.226 "<command>"
```

Auth is via **Pageant** holding `brokilodeluxe.ppk`. Never accept a password or
passphrase in chat — if Pageant is not loaded, launch it so its own dialog
prompts. Repeated failed attempts trip fail2ban.

`MSYS_NO_PATHCONV=1` is required from Git Bash or paths get mangled.

To push a local file up (byte-exact, no heredoc escaping problems):

```bash
plink -batch -hostkey "..." user@host "cat > /tmp/x.py" < localfile
```

Avoid bash heredocs for anything containing backslashes — they get stripped.
Quoted heredocs (`<<'EOF'`) are safe but `cat >` is safer still.

### MySQL (on the server)

```bash
mysql --defaults-file=/home/brokilodeluxe/itemforge/my.cnf -N -B -e "SELECT 1"
```

Schemas:

| schema | purpose |
|---|---|
| `lplusworld` | **PROD** world DB |
| `lplusdevworld` | dev world DB |
| `dbc` | DBC mirrors, `<table>_lplus` (30 of them) + `itemset_legionnaire` |
| `helper` | stored procs (`sp_ScaleItemToIlvl_*`), `helper.forge_item` registry |

**Do not use `mysql -B` (tab-separated) to read tables containing text.** Spell
descriptions and area names contain tabs/newlines and silently shift columns.
Use `mysql --xml --raw` and parse the XML.

Information_schema column names come back UPPERCASE on MySQL 8 — alias them
explicitly (`SELECT table_name AS t`).

---

## 2. Servers

| service | world DB | data/dbc |
|---|---|---|
| `legionnaireplusworld.service` | `lplusworld` | `~/wow/servers/tc-legionnaireplus/data/dbc` |
| `legionnaireplusdevworld.service` | `lplusdevworld` | `~/wow/servers/tc-lplus-dev/data/dbc` |

- Passwordless sudo exists **only** for `legionnaireplusdevworld.service`.
  Prod restarts are the user's to run.
- **SOAP is disabled on prod** (`SOAP.Enabled = 0`), so there is no remote
  console. GM commands must be typed in game.
- `Spell.dbc` and other DBCs are read once at startup and are **not**
  runtime-reloadable. A new spell needs a worldserver restart.
- `item_template` IS reloadable: `.reload item_template`.

Source tree (Windows): `C:\Projects\Gamedev\wow\servers\tc-lplus`, branch
`LEGIONNAIRE_PLUS`. No compiler in the agent sandbox — C++ changes must go
through Jenkins.

---

## 3. DBC architecture — the part that causes most bugs

Each DBC exists in up to **five** places:

1. `dbc.<table>_lplus` — MySQL mirror
2. `~/itemforge/dbc/` — the forge's copy (what the forge reads and writes)
3. `~/wow/servers/tc-legionnaireplus/data/dbc/` — PROD server
4. `~/wow/servers/tc-lplus-dev/data/dbc/` — dev server
5. inside the client patches (two streams — see §4)

### Which side is authoritative is PER TABLE

There is no global rule, and assuming one destroys data.

- **`ItemDisplayInfo`, `Item`** — the Item Forge writes the *binaries* directly
  and has never written the mirror. Binary wins.
- **`ItemSet`** — the **binary** is authoritative and the SQL mirror is a
  *partial* copy: binary 566 sets vs table 529. Generating the DBC from the
  table would silently drop 37 sets. Edit sets through the forge
  (`/api/setedit`), which writes the binary and mirrors to SQL. `set_sql_sync`
  only ever pushes, never pulls.
- **`AreaTable`, `BattlemasterList`, `Map`, most others** — edited in SQL first,
  so a difference just means "not regenerated yet". Mirror wins.

### `dbcgen.py` — mirror → binary

`~/wow/tools/dbcgen/dbcgen.py`, config `dbcgen.conf` (mode 0600, contains a
plaintext MySQL password — don't echo it). Its `[copy]` section writes generated
files into **both** servers' `data/dbc` automatically.

```bash
cd ~/wow/tools/dbcgen
python3 dbcgen.py --no-copy --out /tmp/out ItemDisplayInfo Item
```

- Maps table columns to DBC fields **by name**, from
  `WotLK 3.3.5 (12340).xml`. A missing column makes it fail outright (fail-safe).
- `CreatureModelData` and `Vehicle` mirrors lack required columns, so dbcgen
  **cannot** build those tables at all.
- **A row-loss guard was added 2026-08-11**: before writing, it reads the file
  it would replace and refuses if that file holds record ids the MySQL table
  does not, naming the ids. Override with `--allow-row-loss`.

### `dbc_mirror_sync.py` — binary → mirror (the inverse)

`~/wow/tools/dbcgen/dbc_mirror_sync.py`. Report mode is read-only:

```bash
python3 ~/wow/tools/dbcgen/dbc_mirror_sync.py            # report all mirrors
python3 ~/wow/tools/dbcgen/dbc_mirror_sync.py --apply    # fix the safe ones
```

Only syncs the binary-authoritative allowlist (`ItemDisplayInfo`, `Item`,
`TaxiPath`, `TaxiPathNode`); everything else is reported and left alone.
Mirrors are MyISAM, so it dumps to `~/itemforge/registry-backups/` first.

Known outstanding: **`dbc.spellicon_lplus` has lost every backslash**
(`InterfaceIconsTrade_Engineering`, verified by `HEX()` — no `0x5C`).
Regenerating SpellIcon would break every icon in the game. The row-loss guard
does NOT catch this — it compares ids, not values.

### `Spell.dbc` — has its own generator

```bash
cd ~/itemforge/app/itemforge
~/itemforge/venv/bin/python spell_dbc.py \
  --defaults ~/itemforge/my.cnf --out /tmp/Spell.dbc
```

232 SQL columns → 234 binary fields (two `bigint` columns each pack into two
32-bit fields). Verified: all existing rows regenerate **byte-identically**, so
a full rebuild from the mirror is safe. Deploy the result to all three
`data/dbc` dirs, then into both patch streams.

---

## 4. Client patches — TWO STREAMS, and load order

### The two streams

| path | served as | notes |
|---|---|---|
| `/var/www/html/downloads/patches/` | live | `patch-Y.zip`, `patch-enUS-8.zip`, … |
| `/var/www/html/downloads/patches/itemforge/` | **test** | `patch-*-test.zip` |

**The `-test` suffix is stripped on install.** `patch-enUS-T-test.zip` contains
`patch-enUS-T.MPQ` and lands in the client as `patch-enUS-T.MPQ`. This is the
single biggest trap in the whole setup: the test stream carries full copies of
`Spell.dbc`, `Item.dbc`, `ItemDisplayInfo.dbc`, `ItemSet.dbc` and three
SpellVisual tables, and **T outranks 8**, so anything published only to the live
patch is invisible until the test patch is refreshed too.

> Publish DBC changes to **both** streams, or refresh/remove the test patch.

### Load order (client precedence — higher wins)

`_priority()` in `recolor.py`: `patch-<digit>` → `10 + digit`;
`patch-<letter>` → `20 + (letter - 'a' + 1)`.

So: `enUS-7`=17 < `enUS-8`=18 < `enUS-9`=19 < `enUS-A`=21 < `enUS-T`=40.

**Letters outrank digits.** Alphabetical order is NOT load order.

Client dir: `C:\Projects\Gamedev\wow\clients\centurion\Data\enUS\`
(locale patches) and `...\Data\` (art patches).

### Publish recipe (what `write_into_patch` does, replicated by hand)

1. Extract the inner `.MPQ` from the `.zip`
2. `smpq -a -f <mpq> <relative paths>` with `cwd` = a stage dir
   (internal names come out backslash-separated and match automatically)
3. Re-zip **DEFLATED** with the entry named for the REAL patch
   (`patch-Y.MPQ`, not `patch-Y-test.MPQ`)
4. Bump the sibling `.version` file (format `1.NNNNN`) — **last**, so a failure
   never advertises a half-written archive
5. Verify by extracting the file back OUT of the rebuilt archive

Art lives in `patch-Y` (base) and `patch-Z` (**registry items only** — user's
explicit rule). Data lives in `patch-enUS-8` (+ the test `patch-enUS-T`).

`smpq` error 87 = the WoW client is holding the archive open. Repeated writes
leave orphaned blocks; `compact()` reclaims them.

---

## 5. Item Forge

- Runs on the server, `127.0.0.1:8770`, behind Apache.
- Auth header: **`X-Forge-Token`**, value from `~/itemforge/token`.
- Started with `--assets ~/itemforge/app/itemforge/bundle` — it reads art from a
  pre-extracted 1.4 GB bundle (33k BLPs), **not** from client MPQs. The base
  Earthfury/stock art IS in the bundle but is NOT in any published patch.
- `recolor.py` has Windows paths (`C:\Projects\...`) that never resolve on
  Linux; the `--assets` mode replaces `recolor.fetch` wholesale.

**Four copies of the UI exist and must stay byte-identical:**

```
/var/www/html/forge-assets/{app.js,index.html}          <- Apache serves JS
~/itemforge/app/itemforge/static/{app.js,index.html}    <- backend serves HTML
```

When patching them: validate the GENERATED text with `node --check` **before**
writing (checking the deployed file afterwards is worthless), and never
substitute a bare `\n` into JS source — that blanked the whole page once.

Registry: `helper.forge_item`. Patch-day promotion is `/api/patchday`
(per-item, approved only). `lplusdevworld` is wiped from `lplusworld` on patch
days, which is why the registry stores a replayable `share_state`.

---

## 6. Item level tuning — two behaviours that will mislead you

`helper.sp_ScaleItemToIlvl_SimpleGreedy_v2(entry, target_ilvl, apply,
scale_auras, keep_bonus_armor, scale_unknown_auras)`
(`_Dev` suffix = `lplusdevworld`; unsuffixed = `lplusworld`).

1. **It exits early when the stored `ItemLevel` already equals the target.**
   Calling it at 66 on an item stamped 66 does *nothing*. To make it actually
   recompute, set `ItemLevel` to something else first.
2. **Its writes have escaped `START TRANSACTION … ROLLBACK`** on an InnoDB
   table (observed: a test left an item at ilvl 80 permanently). The proc
   contains no `COMMIT` of its own and this is unexplained — do not trust
   transactional previews around it. Dump and restore instead.

Useful facts: primary stats (types 3,4,5,6,7) all weight 230, so moving points
between them is budget-neutral. Spell-power auras ARE priced, ~5 spell power per
stat point — but only visible with `scale_auras=1` **and** a forced recompute.

Classic itemisation: there is no spell power stat. Use the equip aura family
`Increase Spell Dam N` = `EffectAura_1=13` (damage, school mask 126) +
`EffectAura_2=135` (healing) at equal values. **Spell names are unreliable** —
27925 is called "Increase Healing 20" and grants 11. Always read
`EffectBasePoints`. Filter to `Name_Lang_enUS LIKE 'Increase Spell Dam%'` or you
will pick up consumables like 28273 Bloodthistle that share the aura pair.

---

## 7. Current state of in-flight work

### Goblin Workshop (incomplete)

| piece | state |
|---|---|
| `creature_template` 900116 "Goblin Workshop" | in both worlds, `ScriptName npc_rts_building`, model 40000 |
| `gameobject_template` 900001 (collision shell) | in both worlds |
| `src/server/scripts/Custom/rts_building.cpp` | committed; the AI summons the shell itself |
| spell **90216** "Summon Goblin Workshop" | created: effect 28 → 900116, SummonProperties 64, `ImplicitTargetA=32`, `Targets=64` (ground reticle), `RangeIndex=4`, `DurationIndex=21` (permanent), icon 1 |
| deployed | mirror + all 3 `data/dbc` + `patch-enUS-8` v1.00213 + `patch-enUS-T-test` v1.00012 |

**BLOCKED:** 90216 has **no `SkillLineAbility` row**, so `.learn` succeeds but it
never appears in a spellbook. Needs a row in `dbc.skilllineability_lplus`
(next free id after 22016) with a `SkillLine` + `ClassMask` — *undecided who
should be able to cast it*. Then regenerate `SkillLineAbility.dbc` via dbcgen
and publish to both streams. **Prod also needs a worldserver restart** to load
the new `Spell.dbc`.

### Rotar's Battlegear (done)

Entries 100664–100671, displays 68835–68842, ItemSet 1045, textures tagged
`_Superwofl` (misspelling is load-bearing — renaming orphans the art).
Enhancement set: Strength 149, Stamina 127, Intellect 64, spell power 75
(vs Stormdancer the elemental set at 104), all ilvl 66.

### Diminished debuff

Spell **90201** (+ helper 90202), used by Violet Hold clones via
`PlayerbotVhrWaveDriver.cpp:166`. Stacks = percentage removed. Base points are
PER STACK (`AuraEffect::CalculateAmount` multiplies by stack count after script
handlers). As of 2026-08-12 it carries `SpellVisualID_1 = 15457` — the Tabard of
the Lightbringer glow — so shrunken clones are findable.

---

## 8. Open items

- `dbc.spellicon_lplus` backslash corruption (§3)
- Forge's `dbc/ItemDisplayInfo.dbc` and `Item.dbc` are dated Aug 9 while the
  servers' are newer — the next forge export will republish the test patch from
  those stale copies and shadow live data again
- ItemSet 1045's set bonuses reference spells 21895 / 90125 / 90126 — unverified
- Prod worldserver restart pending for spell 90216
