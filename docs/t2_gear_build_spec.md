# T2 PvP gear — build specification

Task: create **22 armour sets, 8 pieces each** (176 items) and wire each set's
three bonuses. The bonus spells already exist and are listed per set below — do
not create them. Build only the items and the `ItemSet` rows.

---

## 1. What a set looks like

Copy the shape of the shipped T1 sets. Reference set: **1052 "Runic Raiments"**,
items 100728‑100735.

**Eight pieces, one per slot.** `InventoryType` values:

| InventoryType | Slot |
|---|---|
| 1 | Head |
| 3 | Shoulder |
| 20 | Chest (robe) — use 5 for a non‑robe chest |
| 9 | Wrist |
| 10 | Hands |
| 6 | Waist |
| 7 | Legs |
| 8 | Feet |

No weapons, no rings, no trinkets, no cloak.

**Per‑item constants**, matching T1:

| Column | Value |
|---|---|
| `class` | 4 (armour) |
| `subclass` | 1 cloth · 2 leather · 3 mail · 4 plate (per set, below) |
| `ItemLevel` | 66 |
| `RequiredLevel` | 0 |
| `quality` | 4 (epic) |
| `itemset` | the set's ItemSet id |

Stats go in `stat_type1..10` / `stat_value1..10`. Common `stat_type` values:
`3` Agility · `4` Strength · `5` Intellect · `6` Spirit · `7` Stamina ·
`12` Defence · `31` Hit · `32` Crit · `36` Haste · `44` Armour Pen ·
`45` Spell Power · `47` Spell Pen · `43` Mana per 5.

Also set `armor` per slot, as T1 does (e.g. head 83 at ilvl 66 cloth).

---

## 2. Free id ranges

| Thing | Highest used | Start at |
|---|---|---|
| `item_template.entry` | 100756 | **100800** |
| `ItemSet` id (`dbc.itemset_legionnaire`) | 1054 | **1055** |

Keep each set's 8 item entries contiguous, and keep sets in the order below, so
the ranges stay readable.

---

## 3. Wiring the bonuses

Bonuses attach through **`ItemSet.dbc`** (mirror table `dbc.itemset_legionnaire`),
not through item spells. Per set row:

- `ItemID_1..8` — the eight item entries
- `SetSpellID_1` / `SetThreshold_1` — first bonus + piece count
- `SetSpellID_2` / `SetThreshold_2` — second
- `SetSpellID_3` / `SetThreshold_3` — third
- `SetSpellID_4..8` / thresholds — leave 0
- `Name_Lang_enUS` — the set name

Example, from live set 1052: `SetSpellID_1 = 90149, SetThreshold_1 = 3`,
`SetSpellID_2 = 90161, SetThreshold_2 = 5`, `SetSpellID_3 = 90162, SetThreshold_3 = 8`.

**Thresholds are 3 / 5 / 8 for every set except the two shaman sets, which are
3 / 4 / 8.** They are listed per set below; use the listed numbers.

After editing the mirror, regenerate and publish `ItemSet.dbc` and `Item.dbc`
through the normal DBC pipeline.

---

## 4. The sets

Each entry gives: armour type · appearance reference · the three bonuses with the
**spell id to put in `SetSpellID_n`** and the threshold for `SetThreshold_n` ·
the stat distribution to build to.

---

### The Rend Set — plate (subclass 4)
https://www.wowhead.com/wotlk/outfit=178735/stormwind-guard-1

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90300 | Your Rend costs 3 less rage |
| 5 | 90301 | Your Rend is now a new spell (double Rend's damage, same spell mask, damage paid by yards moved/displaced rather than over the duration — over 63 yards instead of 21 seconds; lasts 1 minute) |
| 8 | 90302 | Your Intercept now knocks targets back instead of stunning |

**Stats:** regular warrior stats

---

### The Unarmed Set — plate (subclass 4)
https://www.wowhead.com/wotlk/transmog-set=1182/angrorosh-plate-recolor#modelviewer

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90303 | While unarmed, you deal 50% more damage and are immune to disarm. You may use all your Warrior abilities without any item restrictions |
| 5 | 90304 | You attack 30% faster while unarmed |
| 8 | 90305 | While unarmed your crits stun for 1 second |

**Stats:** regular dps warrior stats

---

### The Consecration Set — plate (subclass 4)
https://www.wowhead.com/wotlk/dressing-room#fM80z0zN89c8u8VkZ8G8VRs8I8VRh8N8VRp8A8VRe877gazT808mTk87cmTM87VmTa808mTc808mTm808mTo808mTR87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90306 | Reduces the mana cost of your Consecration by 30% |
| 5 | 90307 | Enemies within 3 yards of the center of your Consecration take double damage from it |
| 8 | 90308 | Your Consecration is centered around your character and moves with you |

**Stats:** regular healing paladin stats

---

### The Holy Shock Set — plate (subclass 4)
https://www.wowhead.com/wotlk/dressing-room#fm80z0zN89c8F8Vqp8H8Vqv8J8VqK8K8VqX8O8Vb77eRLQ808RfI87cRNi87kRsH808RFo808RhI808dsZ87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90309 | Reduces the mana cost of Holy Shock by 5% |
| 5 | 90310 | Your Judgement spell will now cause your target to take an additional +3% Crit chance from all Holy Spells for 10 sec |
| 8 | 90311 | When your Holy Shock critically hits, the cooldown is instantly reset. This effect cannot happen more than once every 10 seconds |

**Stats:** spell damage plate stats

---

### The Frost Shock Set — mail (subclass 3) · thresholds **3 / 4 / 8**
https://centurionpvp.com/itemforge#eyJ2IjoxLCJzIjoib25lIiwiZyI6WzEsIjJhNmY4ZiIsImNmZTZmMiIsImNmZTZmMiIsImNmZTZmMiIsIjdkOTNhMyIsMSwwLjMsMSwwLjIyLDEuMSwxXSwiZSI6eyIxIjoyMzAzMywiMyI6MjI5NjcsIjUiOjIyNjY0LCI3IjoyMjcwMiwiOCI6NDM1OTUsIjEwIjoyMjY2Nn0sInAiOnsiMSI6WzEsIjJhNmY4ZiIsImNmZTZmMiIsImNmZTZmMiIsImNmZTZmMiIsIjdkOTNhMyIsMSwwLjMsMSwwLjIyLDEuMSwxXSwiMyI6WzEsIjJhNmY4ZiIsImNmZTZmMiIsImNmZTZmMiIsImNmZTZmMiIsIjdkOTNhMyIsMSwwLjMsMSwwLjIyLDEuMSwxXSwiOCI6WzEsIjJhNmY4ZiIsIjYyYTBlYSIsImNmZTZmMiIsImNmZTZmMiIsIjM1ODRlNCIsMSwwLjMsMSwwLjIyLDEuMSwxXSwiMTAiOlsxLCIyYTZmOGYiLCJjZmU2ZjIiLCJjZmU2ZjIiLCJjZmU2ZjIiLCI3ZDkzYTMiLDEsMC4zLDEsMC4yMiwxLjEsMV19LCJrIjoiOTNiMGExNTVkODQ3NDNiMiIsImQiOnsiMSI6eyJzdCI6W1s3LDIxXSxbMywxNl1dLCJyIjpbMCwwLDAsMzksMCwwXSwicSI6NCwiYWIiOjM1OH0sIjMiOnsic3QiOltbNywyMl1dLCJyIjpbMCwwLDAsMjksMCwwXSwicSI6NCwiYWIiOjM0Mn0sIjUiOnsic3QiOltbNywyMl1dLCJzcCI6W1sxNDA4OSwxXV0sInIiOlswLDAsMCwzNiwwLDBdLCJxIjo0LCJhYiI6NDM0fSwiNyI6eyJzdCI6W1s3LDI1XSxbMywxNF1dLCJyIjpbMCwwLDAsMzYsMCwwXSwicSI6NCwiYWIiOjM4Mn0sIjgiOnsic3QiOltbNywxMjldXSwiciI6WzAsMCwwLDg2LDAsMF0sInEiOjQsImFiIjo4ODR9LCIxMCI6eyJzdCI6W1s3LDIwXV0sInNwIjpbWzkzMzEsMV1dLCJyIjpbMCwwLDAsMjcsMCwwXSwicSI6NCwiYWIiOjI4MX19fQ

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90312 | Reduces the mana cost of your Frost Shock by 30% |
| **4** | 90313 | Your Frost Shock is off cooldown with your other shocks |
| 8 | 90314 | You can purge your totems to grant it a 500 hp shield. Doing this puts purge on a 10 second cooldown |

**Stats:** Regular resto shaman stats

---

### The Flame Shock Set — mail (subclass 3) · thresholds **3 / 4 / 8**
https://centurionpvp.com/itemforge#eyJ2IjoxLCJzIjoib25lIiwiZyI6WzAsIjU5NDUyYyIsIjU5NDUyYyIsIjZmOWUzZiIsImIwM2E1ZSIsIjZhNjU1YyIsMCwwLjM1LDEsMC4yLDEuMiwxXSwiZSI6eyIxIjo3NzE5LCIzIjoxNDc3NiwiNSI6MjI4NzYsIjYiOjIxNDYzLCI3IjoxMzEyOSwiOCI6MjAxODEsIjkiOjIyNDcxLCIxMCI6MTk2OTJ9LCJwIjp7IjEiOlsxLCJjNjQ2MDAiLCJjMDFjMjgiLCJjNjQ2MDAiLCJjNjQ2MDAiLCJjNjQ2MDAiLDEsMCwwLjI1LDAuNDgsMiwxXSwiMyI6WzEsImM2NDYwMCIsIjk5YzFmMSIsIjk5YzFmMSIsImY2NjE1MSIsIjZhNjU1YyIsMSwxLDEsMC4zNCwyLDFdLCI1IjpbMSwiYzY0NjAwIiwiZGM4YWRkIiwiMDAwMDAwIiwiYjAzYTVlIiwiMDAwMDAwIiwxLDAuMzUsMC44NSwwLjIsMS4yLDFdLCI2IjpbMSwiYzY0NjAwIiwiNTk0NTJjIiwiNmY5ZTNmIiwiYjAzYTVlIiwiYzM0MjAzIiwxLDAuMzUsMSwwLjIsMS4yLDFdLCI3IjpbMSwiYzM0MjAzIiwiZGM4YWRkIiwiNmY5ZTNmIiwiMzU4NGU0IiwiYzY0NjAwIiwxLDEsMC41LDAuMTYsMiwxXSwiOCI6WzEsImM2NDYwMCIsImMzNDIwMyIsImM2NDYwMCIsImM2NDYwMCIsImM2NDYwMCIsMSwwLDEsMC4wNCwyLDFdLCI5IjpbMCwiNTk0NTJjIiwiNTk0NTJjIiwiNmY5ZTNmIiwiYjAzYTVlIiwiNmE2NTVjIiwwLDAuMzUsMSwwLjIsMS4yLDFdLCIxMCI6WzEsImM2NDYwMCIsIjU5NDUyYyIsIjZmOWUzZiIsImIwM2E1ZSIsIjZhNjU1YyIsMCwxLDEsMC4yOCwyLDFdfSwiayI6ImFjOTQyMDJkY2IyMTFmY2YiLCJkIjp7IjEiOnsic3QiOltbNyw4XSxbNCwxM11dLCJzcCI6W1s3NTk3LDFdXSwicSI6MywiYWIiOjIxM30sIjMiOnsic3QiOltbNCw3XSxbNyw1XSxbNiwzXV0sInEiOjIsImFiIjoxODF9LCI1Ijp7InN0IjpbWzcsMThdLFs1LDE4XSxbNCwxN11dLCJzcCI6W1s3NTk3LDFdXSwicSI6MywiYWIiOjM5OH0sIjYiOnsic3QiOltbMywyMF0sWzcsMTldLFs1LDEwXV0sInNwIjpbWzc1OTcsMV0sWzE1NDY0LDFdXSwicSI6NCwiYWIiOjI1OH0sIjciOnsic3QiOltbNCwxOV0sWzcsNV1dLCJyIjpbMCwxMCwwLDAsMCwwXSwicSI6MywiYWIiOjIxOH0sIjgiOnsic3QiOltbNCwxNF0sWzcsOF0sWzMsMTJdLFs1LDhdXSwic3AiOltbMjM5OTAsMV1dLCJxIjozLCJhYiI6NDUyfSwiOSI6eyJzdCI6W1s3LDE0XSxbNSwxNF1dLCJzcCI6W1s4MTY3OSwxXSxbMjEzNjEsMV1dLCJxIjo0LCJhYiI6MTk4fSwiMTAiOnsic3QiOltbNywxN10sWzMsMTBdXSwic3AiOltbNzU5NywxXV0sInEiOjMsImFiIjoyMzh9fX0

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90315 | Reduces the mana cost of your Flame Shock by 30% |
| **4** | 90316 | Your Flame Shock is off cooldown with your other shocks |
| 8 | 90317 | You can purge your totems to grant yourself 2 stacks of Flurry, killing them and putting purge on a 15 second cooldown |

**Stats:** Regular enhance shaman stats

---

### The Imp Set — cloth (subclass 1)
https://www.wowhead.com/wotlk/dressing-room#fM80z0zN89c8u8VkZ8G8VRs8I8VRh8N8VRp8A8VRe877grPs808xP87cmkk87VsHz808oyN87cmY7MRjd87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90318 | Your Shadow Bolt is now fire damage and uses firebolt's graphic |
| 5 | 90319 | Your Imp has 100% more mana and 10% more health |
| 8 | 90320 | If you die before your Imp, you revive with his hp at his location. This effect can only happen once every 120 seconds |

**Stats:** Regular warlock stats with reduced budget allocated to stamina

---

### The Life Tap Set — cloth (subclass 1)
https://www.wowhead.com/wotlk/dressing-room#fM80z0zN89c8u8VkZ8G8VRs8I8VRh8N8VRp8A8VRe877vxP87cmkk87VsHz808oyN87cmY7MRjd87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90321 | The closer your mana is to half, the less damage you take, up to 5% damage reduction |
| 5 | 90322 | You no longer regenerate mana through any means other than Life Tap. Life Tap mana gain doubled |
| 8 | 90323 | Reduce the global cooldown of Life Tap by 0.5 seconds |

**Stats:** Regular warlock stats

---

### The Hurricane Set — leather (subclass 2)
https://www.wowhead.com/wotlk/dressing-room#fo80R0zN89c8za8Vrx8zd8Vr28zr8Vfm8zf8Vfk8ox8VrW877gV2G808mwd87cMS2808bfd87VM1I808oNF87czNG808MP187MkTx87o

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 89767 | 70% pushback protection on hurricane |
| 5 | 89760 | You can move at 15% speed while casting hurricane |
| 8 | 89766 | You are unstoppable while casting hurricane rank 3 |

**Stats:** Regular balance druid stats

> Note the ids here are 897xx, not 903xx — this set reuses three bonuses that
> already existed in the core.

---

### The Moonfire Set — leather (subclass 2)
https://www.wowhead.com/classic/dressing-room#fo80q0zM89c8za8Vrx8ox8VrW8zd8Vr28zr8Vfm8zf8Vfk877gzqEq808cUY87cz1E87kcVt808cVx808cVn808cVy87qh

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90327 | Your mangle effect increases the damage the target takes from your spells |
| 5 | 90328 | Your moonfire generates 1 combo point if the target isn't already affected by your moonfire. Your moonfire increases the damage your target takes from your melee abilities |
| 8 | 90329 | Shifting from moonkin directly to catform is free |

**Stats:** All of the gear has pure, balanced rainbow stats with no critical strike or hit on it. (no green text)

---

### The Raptor Strike Set — mail (subclass 3)
https://www.wowhead.com/wotlk/dressing-room#fm80z0zN89c8F8Vqp8H8Vqv8J8VqK8K8VqX8O8Vb77ed12808cxD87cc0d87Vc0f808c0h808mRJ808c0G808c0r87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90330 | Your Raptor Strike cooldown is reduced by 1 second |
| 5 | 90331 | Gain 1 melee attack power per point of Strength |
| 8 | 90332 | While Aspect of the Monkey is active, your melee attacks have a 5% chance of increasing melee attack speed by 30% for 12 sec |

**Stats:** Regular hunter stats but half of the Agility budget would be shifted into strength.

---

### The Trap Set — mail (subclass 3)
https://www.wowhead.com/wotlk/dressing-room#fm80z0zN89c8F8Vqp8H8Vqv8J8VqK8K8VqX8O8Vb77eVjE808mBT87cmBG87VMPX808mBA808mBi808mBp808mBL87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90333 | Reduces the mana cost of your Frost and Freezing Trap spells by 50% |
| 5 | 90334 | Fire damage melts your Freezing Trap, turning it into your Frost trap effect. Your Serpent Sting now does fire damage instead of nature damage and uses explosive shot's graphic |
| 8 | 90335 | The first time you enter your Frost trap's ice slick, you go 30% faster for 3 seconds |

**Stats:** Regular hunter stats

---

### The Holy Fire Set — cloth (subclass 1)
https://www.wowhead.com/wotlk/dressing-room#fm80z0zN89c8F8Vqp8H8Vqv8J8VqK8K8VqX8O8Vb77ecRh808zAt87czZE87p

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90336 | Increase the range of Holy Fire by 5 yards |
| 5 | 90337 | If you get a critical strike with Smite or Flash Heal, your next Holy Nova is free and has 25% increased critical strike chance |
| 8 | 90338 | If you get a critical strike with Holy Fire, also imbue your main hand weapon with Holy Fire, making its next attack within 30 seconds cast Holy Fire on the target and grant you Spirit Tap |

**Stats:** regular holy priest stats with a heavy focus on spirit

---

### The Shadowform Set — cloth (subclass 1)
https://www.wowhead.com/wotlk/dressing-room#fc80V0zN89c8x8Vsy8t8VsU8g8Va08e8Vaq8v8VaI877gMVx87cd5Y808RY487VoJS808MdD808MdO808cxs808MdF87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90339 | Reduces the global cooldown of Vampiric Embrace by 0.5 seconds |
| 5 | 90340 | You can cast heal in Shadowform, but doing so inflicts 50% of the healing done as self damage |
| 8 | 90341 | Your damage with melee attacks is increased by 300% and restores mana equal to 10% of the damage done |

**Stats:** regular shadow priest stats with a little bit of budget shifted to strength and attack power on each piece

---

### The Ice Block Set — cloth (subclass 1)
https://www.wowhead.com/wotlk/dressing-room#fm80z0zN89c8F8Vqp8H8Vqv8J8VqK8K8VqX8O8Vb77ezZW808M3A87cmb7T

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90342 | Every target hit by your cone of cold reduces the cooldown of ice block by 3 seconds |
| 5 | 90343 | If your ice block lasts the full duration, foes within 10 yards are encased in ice for 6 seconds. Damage breaks this effect |
| 8 | 90344 | Your ice block reflects 50% damage back at attackers |

**Stats:** regular mage stats

---

### The Fiery Payback Set — cloth (subclass 1)
https://www.wowhead.com/wotlk/dressing-room#fM80z0zN89c8u8VkZ8G8VRs8I8VRh8N8VRp8A8VRe877gRY5808xB87cczt87VoNm808Vg2808VOn808czj808czy87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90345 | When struck in combat, you have a chance to gain 20 spell power for 15 seconds, stacking up to 5 times |
| 5 | 90346 | Your Blazing Speed resets the cooldown of your Fire Blast |
| 8 | 90347 | Your Fiery Payback equips you with the melee weapons that were disarmed temporarily (while the target is disarmed) and grants you attack power equal to double your spell power. You are immune to disarm during this time |

**Stats:** regular mage stats with some bonus armor on each piece

---

### The Poison Utility Set — leather (subclass 2)
https://www.wowhead.com/wotlk/transmog-set=127/emblazoned-garb#modelviewer
*(appearance marked "revisit" in the design doc)*

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90348 | Your crippling poison turns into "chilling poison", snaring the target for 55% movement speed and reducing the time between attacks by 15% |
| 5 | 90349 | Your sprint turns into "ice skate", granting 45% movement speed over 18 seconds and leaving an ice trail behind you, slowing enemies by 30% |
| 8 | 90350 | Your cold blood turns into "colder blood". Instead of guaranteeing your next crit, it roots the target in place for 3 seconds |

**Stats:** regular rogue stats

---

### The Deadly Poison Set — leather (subclass 2)
https://www.wowhead.com/wotlk/dressing-room#fm80z0zN89c8F8Vqp8H8Vqv8J8VqK8K8VqX8O8Vb77eROC808mge87cYJ87Vmgl808ROD808kJK808Yw808mgy87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90351 | +25 spell penetration |
| 5 | 90352 | You heal for 20% of the damage your poisons do |
| 8 | 90353 | When your Deadly Poison hits 5 stacks, it explodes for 80% of the damage it would deal instantly, removing all the stacks |

**Stats:** regular rogue stats but with some of the budget shifted to +nature damage

---

### The Frost Plate Set — plate (subclass 4)
https://www.wowhead.com/wotlk/dressing-room#fm80z0zN89c8F8Vqp8H8Vqv8J8VqK8K8VqX8O8Vb77ebiv808cMz87cr3R87VaTN808rRk808dFu808rkz808rzP87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90354 | Adds 8 Frost Damage to your attacks |
| 5 | 90355 | You gain Frost Aura, inflicting 25 Frost Damage every 2 seconds to all nearby enemies within 5 yards and slowing them by 10%. This effect will break Polymorph and Charm Effects |
| 8 | 90356 | When killed, you become a tomb of ice for 15 seconds, shielding anyone who stands behind it |

**Stats:** regular prot warrior stats

---

### The Momentum Mail Set — mail (subclass 3)
https://www.wowhead.com/wotlk/dressing-room#fm80z0zN89c8F8Vqp8H8Vqv8J8VqK8K8VqX8O8Vb77eqWP808qUy87cdcD87VaSb808bJt808aSV808aBW808qJT87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90357 | All snares and roots last 10% less duration |
| 5 | 90358 | Running in a straight line for 3 seconds builds momentum: you move an extra 15% faster. This can stack up to 4 times. If your character stops, turns, or strafes for any reason, all stacks are removed |
| 8 | 90359 | You gain 5% damage reduction for each stack of momentum |

**Stats:** regular hunter stats with a slightly heavier budget focus of mana per 5 and intelligence

---

### The Recovery Leather Set — leather (subclass 2)
https://www.wowhead.com/wotlk/dressing-room#fM80z0zN89c8u8VkZ8G8VRs8I8VRh8N8VRp8A8VRe877gML2808qWW87cooK87Vz3y808z3g808m4s808z3e808kBA87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90360 | Increases the amount of HP you gain from your Eat spell by 25% |
| 5 | 90361 | You mount 1.5 second faster |
| 8 | 90362 | Reduces the duration of your Recently Bandaged debuff by 40 seconds |

**Stats:** regular rogue stats

---

### The Casting Cloth Set — cloth (subclass 1)
https://www.wowhead.com/wotlk/dressing-room#fz80z0zN89c8s8VVX8a8Vom8q8Vow8b8Von8d8VoC877goLD808xB87cc0K808OL87kkJn808pW808MWt87q

| Threshold | Spell id | Bonus |
|---|---|---|
| 3 | 90363 | Reduces the mana cost of all spells by 3% |
| 5 | 90364 | Cancelling a cast after at least 1 second of casting makes your next spell within 4 seconds cast 10% faster |
| 8 | 90365 | When an enemy uses an interrupt on you while you aren't casting, your next spell within 5 seconds casts 25% faster |

**Stats:** regular mage stats

---

## 5. One constraint that is not obvious

The Moonfire Set's 8pc (`90329`) **only works when granted through `ItemSet.dbc`**.
Its shapeshift restriction is applied by `Player::ApplyEquipSpell` on an
`ItemSetEff`; granting it any other way silently loses that restriction. Since
this spec uses `ItemSet.dbc` throughout, that is satisfied — just do not move it
to a different delivery mechanism.
