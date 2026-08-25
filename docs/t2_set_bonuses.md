# T2 PvP set bonuses — design + implementing spell ids

Every bonus, with the spell(s) that actually implement it. Ids in **bold** are the
**carrier** — the inert aura the set grants and every script keys on with
`HasAura()`. Ids after it are helpers: buffs, debuffs, damage vehicles, wrappers
and visuals.

Carriers live at 90300‑90365, their first‑round helpers at 90366‑90394, and the
2026‑08‑19/20 rework helpers at 90400‑90538. Thresholds are 3/5/8 unless noted.

Sets are named functionally for now. Stat lines are the design doc's, verbatim.

Status key: **✅ live** · **⚠ built, unverified**

Every bonus listed has a script. A carrier being an inert DUMMY passive does NOT
mean the bonus is unbuilt — that is true of every carrier in this system; the work
lives in scripts keyed on `HasAura(carrier)` and bound through `spell_script_names`.
Check that table, not the carrier's effects.

---

## Warrior

### The Rend Set
| | Bonus | Ids |
|---|---|---|
| 3 | Your Rend costs 3 less rage | **90300** ✅ |
| 5 | Your Rend is now a new spell (double Rend's damage, same spell mask, damage paid by yards moved/displaced rather than over the duration — over 63 yards instead of 21 seconds; lasts 1 minute) | **90301** + `90366` ✅ |
| 8 | Your Intercept now knocks targets back instead of stunning | **90302** + `90367` ⚠ |

> regular warrior stats

*`GAPING_WOUND_YARDS_PER_STACK = 3.0f`; 21 stacks × 3 yd = 63. Knockback reaches
playerbots — both `Unit::KnockbackFrom` and `MotionMaster::MoveKnockbackFrom` have
socketless‑bot exceptions and every managed bot here is a virtual session. Stock
`EffectKnockBack` still refuses a **rooted or stunned** target, bots and players alike.*

### The Unarmed Set
| | Bonus | Ids |
|---|---|---|
| 3 | While unarmed, you deal 50% more damage and are immune to disarm. You may use all your Warrior abilities without any item restrictions | **90303** + `90217` ⚠ |
| 5 | You attack 30% faster while unarmed | **90304** + `90369` ✅ |
| 8 | While unarmed your crits stun for 1 second | **90305** + `90370` ✅ |

> regular dps warrior stats

*`90217` is the **single** aura for the 3pc — damage, disarm immunity and the
weapon‑requirement waiver together. It is also the id in
`Centurion.Unarmed.WaiverAuras` and in the client's `rulerelax.cpp`. `90368` is the
retired split buff: applied by nothing, only torn down.*

---

## Paladin

### The Consecration Set
| | Bonus | Ids |
|---|---|---|
| 3 | Reduces the mana cost of your Consecration by 30% | **90306** ✅ |
| 5 | Enemies within 3 yards of the center of your Consecration take double damage from it | **90307** ⚠ |
| 8 | Your Consecration is centered around your character and moves with you | **90308** + `90523` ⚠ |

> regular healing paladin stats

*The 5pc doubles the **tick itself** via `T2UnitHooks::ApplySanctifiedCore`, called
from `AuraEffect::HandlePeriodicDamageAurasTick` — a dynobject aura has one
`AuraEffect` shared by every unit in it, so a per‑target decision can only be made
after the damage is computed. `90371` is the retired second‑hit helper.*

*The 8pc is the wrapper pattern: wrappers `90526‑90531` (one per rank
26573/20116/20922/20923/20924/27173) cast either the untouched rank or a
visual‑less clone `90532‑90537`. `90523` carries the ring as a **StateKit**
(SpellVisual 21101 → SpellVisualKit 21101 → SVEN 2542), which is why it follows
the model.*

### The Holy Shock Set
| | Bonus | Ids |
|---|---|---|
| 3 | Reduces the mana cost of Holy Shock by 5% | **90309** ✅ |
| 5 | Your Judgement spell will now cause your target to take an additional +3% Crit chance from all Holy Spells for 10 sec | **90310** + `90372` ✅ |
| 8 | When your Holy Shock critically hits, the cooldown is instantly reset. This effect cannot happen more than once every 10 seconds | **90311** ✅ |

> spell damage plate stats

---

## Shaman — thresholds are **3 / 4 / 8** (intentional)

### The Frost Shock Set
| | Bonus | Ids |
|---|---|---|
| 3 | Reduces the mana cost of your Frost Shock by 30% | **90312** ✅ |
| 4 | Your Frost Shock is off cooldown with your other shocks | **90313** ⚠ |
| 8 | You can purge your totems to grant it a 500 hp shield. Doing this puts purge on a 10 second cooldown | **90314** + `90373` ⚠ |

> Regular resto shaman stats

### The Flame Shock Set
| | Bonus | Ids |
|---|---|---|
| 3 | Reduces the mana cost of your Flame Shock by 30% | **90315** ✅ |
| 4 | Your Flame Shock is off cooldown with your other shocks | **90316** ⚠ |
| 8 | You can purge your totems to grant yourself 2 stacks of Flurry, killing them and putting purge on a 15 second cooldown | **90317** ⚠ |

> Regular enhance shaman stats

*The shock‑sharing bonuses are core: `T2SpellHooks` takes the shock off shared
category 19 and re‑syncs the client cooldown after SPELL_GO.*

---

## Warlock

### The Imp Set
| | Bonus | Ids |
|---|---|---|
| 3 | Your Shadow Bolt is now fire damage and uses firebolt's graphic | **90318** + wrappers `90420‑90432`, clones, SpellVisual `90000` ⚠ |
| 5 | Your Imp has 100% more mana and 10% more health | **90319** + `90374` ⚠ |
| 8 | If you die before your Imp, you revive with his hp at his location. This effect can only happen once every 120 seconds | **90320** + `90525`, `90538`, `90375` ⚠ |

> Regular warlock stats with reduced budget allocated to stamina

*The 8pc is **death prevention**, not resurrection: `T2UnitHooks::OnWouldBeLethalDamage`
runs from `Unit::DealDamage` before `Kill()`. Mana is untouched because he never
dies. `90538` carries `DEATH_PERSISTENT` + `UNAFFECTED_BY_INVULNERABILITY` and is
the authoritative cooldown, not just an indicator. `90375` also scales the imp 2×.*

### The Life Tap Set
| | Bonus | Ids |
|---|---|---|
| 3 | The closer your mana is to half, the less damage you take, up to 5% damage reduction | **90321** ✅ |
| 5 | You no longer regenerate mana through any means other than Life Tap. Life Tap mana gain doubled | **90322** ✅ |
| 8 | Reduce the global cooldown of Life Tap by 0.5 seconds | **90323** ✅ |

> Regular warlock stats

---

## Druid

### The Hurricane Set
| | Bonus | Ids |
|---|---|---|
| 3 | 70% pushback protection on hurricane | `89767` ✅ |
| 5 | You can move at 15% speed while casting hurricane | `89760` ✅ |
| 8 | You are unstoppable while casting hurricane rank 3 | `89766` ✅ |

> Regular balance druid stats

*No 903xx carriers — these are the fork's **existing** core boons, keyed in
`Spell.cpp`. The custom rows 90324‑90326/90376 were deleted once that was found.*

### The Moonfire Set
| | Bonus | Ids |
|---|---|---|
| 3 | Your mangle effect increases the damage the target takes from your spells | **90327** + `90377` ⚠ |
| 5 | Your moonfire generates 1 combo point if the target isn't already affected by your moonfire. Your moonfire increases the damage your target takes from your melee abilities | **90328** + `90378` ⚠ |
| 8 | Shifting from moonkin directly to catform is free | **90329** + `90484` ⚠ |

> All of the gear has pure, balanced rainbow stats with no critical strike or hit on it. (no green text)

---

## Hunter

### The Raptor Strike Set
| | Bonus | Ids |
|---|---|---|
| 3 | Your Raptor Strike cooldown is reduced by 1 second | **90330** ✅ |
| 5 | Gain 1 melee attack power per point of Strength | **90331** ✅ |
| 8 | While Aspect of the Monkey is active, your melee attacks have a 5% chance of increasing melee attack speed by 30% for 12 sec | **90332** + `90379` ⚠ |

> Regular hunter stats but half of the Agility budget would be shifted into strength.

### The Trap Set
| | Bonus | Ids |
|---|---|---|
| 3 | Reduces the mana cost of your Frost and Freezing Trap spells by 50% | **90333** ⚠ |
| 5 | Fire damage melts your Freezing Trap, turning it into your Frost trap effect. Your Serpent Sting now does fire damage instead of nature damage and uses explosive shot's graphic | **90334** + wrappers `90460‑90471`, fire clones `90472‑90483` ⚠ |
| 8 | The first time you enter your Frost trap's ice slick, you go 30% faster for 3 seconds | **90335** + `90382` ⚠ |

> Regular hunter stats

---

## Priest

### The Holy Fire Set
| | Bonus | Ids |
|---|---|---|
| 3 | Increase the range of Holy Fire by 5 yards | **90336** ✅ |
| 5 | If you get a critical strike with Smite or Flash Heal, your next Holy Nova is free and has 25% increased critical strike chance | **90337** + `90383` ✅ |
| 8 | If you get a critical strike with Holy Fire, also imbue your main hand weapon with Holy Fire, making its next attack within 30 seconds cast Holy Fire on the target and grant you Spirit Tap | **90338** + `90384` ⚠ |

> regular holy priest stats with a heavy focus on spirit

### The Shadowform Set
| | Bonus | Ids |
|---|---|---|
| 3 | Reduces the global cooldown of Vampiric Embrace by 0.5 seconds | **90339** ✅ |
| 5 | You can cast heal in Shadowform, but doing so inflicts 50% of the healing done as self damage | **90340** + `90501` ✅ |
| 8 | Your damage with melee attacks is increased by 300% and restores mana equal to 10% of the damage done | **90341** + `90500` ⚠ |

> regular shadow priest stats with a little bit of budget shifted to strength and attack power on each piece

*The 5pc prices the **gross** heal, overheal included. The Shadowform toggle is
core: `T2SpellHooks::OnCancelAuraRequest` swallows the client's auto‑unshift
`CMSG_CANCEL_AURA(15473)` and defers a real press by one tick.*

---

## Mage

### The Ice Block Set
| | Bonus | Ids |
|---|---|---|
| 3 | Every target hit by your cone of cold reduces the cooldown of ice block by 3 seconds | **90342** ⚠ |
| 5 | If your ice block lasts the full duration, foes within 10 yards are encased in ice for 6 seconds. Damage breaks this effect | **90343** + `90385` ⚠ |
| 8 | Your ice block reflects 50% damage back at attackers | **90344** + `90400` ⚠ |

> regular mage stats

*⚠ **Ice Block here is `11958`, the classic Frost talent — not the WotLK `45438`.**
52 dev / 51 prod characters hold 11958 and **zero** hold 45438. Both halves of
this set were bound to the wrong id until 2026‑08‑20 and never fired once.*

### The Fiery Payback Set
| | Bonus | Ids |
|---|---|---|
| 3 | When struck in combat, you have a chance to gain 20 spell power for 15 seconds, stacking up to 5 times | **90345** + `90386` ✅ |
| 5 | Your Blazing Speed resets the cooldown of your Fire Blast | **90346** ⚠ |
| 8 | Your Fiery Payback equips you with the melee weapons that were disarmed temporarily (while the target is disarmed) and grants you attack power equal to double your spell power. You are immune to disarm during this time | **90347** + `90387`, `90524` ⚠ |

> regular mage stats with some bonus armor on each piece

*The 8pc triggers on **Fiery Payback's own disarm (64346)** landing, not on Fire
Blast. Fiery Payback is Talent 15 / tab 41 here (ranks 64353/64357). `90524` adds
the off‑hand disarm Fiery Payback itself does not do.*

---

## Rogue

### The Poison Utility Set
| | Bonus | Ids |
|---|---|---|
| 3 | Your crippling poison turns into "chilling poison", snaring the target for 55% movement speed and reducing the time between attacks by 15% | **90348** + enchant `3889`, proc `90513` ⚠ |
| 5 | Your sprint turns into "ice skate", granting 45% movement speed over 18 seconds and leaving an ice trail behind you, slowing enemies by 30% | **90349** + wrappers `90514‑90516`, `90517`, `90518` ✅ |
| 8 | Your cold blood turns into "colder blood". Instead of guaranteeing your next crit, it roots the target in place for 3 seconds | **90350** + `90391`, `90521` ⚠ |

> regular rogue stats

*Cold Blood is a **talent**, so `14177` was rewritten in place rather than wrapped.
The trail's art is SpellVisual 21100 → Kit 21100 → SVEN 21100 (frost trap at 0.15).*

### The Deadly Poison Set
| | Bonus | Ids |
|---|---|---|
| 3 | +25 spell penetration | **90351** ✅ |
| 5 | You heal for 20% of the damage your poisons do | **90352** + `90520` ⚠ |
| 8 | When your Deadly Poison hits 5 stacks, it explodes for 80% of the damage it would deal instantly, removing all the stacks | **90353** + `90519` ⚠ |

> regular rogue stats but with some of the budget shifted to +nature damage

---

## Armour sets

### The Frost Plate Set
| | Bonus | Ids |
|---|---|---|
| 3 | Adds 8 Frost Damage to your attacks | **90354** ✅ |
| 5 | You gain Frost Aura, inflicting 25 Frost Damage every 2 seconds to all nearby enemies within 5 yards and slowing them by 10%. This effect will break Polymorph and Charm Effects | **90355** ⚠ |
| 8 | When killed, you become a tomb of ice for 15 seconds, shielding anyone who stands behind it | **90356** + gameobject `900118` ⚠ |

> regular prot warrior stats

*The tomb is a **door‑type gameobject** (clone of Sindragosa's Ice Tomb, display
6752 = the Naxx/Sapphiron block): impassable client‑side and a native LoS wall,
because its model enters the map's dynamic vmap tree.*

### The Momentum Mail Set
| | Bonus | Ids |
|---|---|---|
| 3 | All snares and roots last 10% less duration | **90357** ✅ |
| 5 | Running in a straight line for 3 seconds builds momentum: you move an extra 15% faster. This can stack up to 4 times. If your character stops, turns, or strafes for any reason, all stacks are removed | **90358** + `90392` ⚠ |
| 8 | You gain 5% damage reduction for each stack of momentum | **90359** ⚠ |

> regular hunter stats with a slightly heavier budget focus of mana per 5 and intelligence

### The Recovery Leather Set
| | Bonus | Ids |
|---|---|---|
| 3 | Increases the amount of HP you gain from your Eat spell by 25% | **90360** ✅ |
| 5 | You mount 1.5 second faster | **90361** ✅ |
| 8 | Reduces the duration of your Recently Bandaged debuff by 40 seconds | **90362** ✅ |

> regular rogue stats

### The Casting Cloth Set
| | Bonus | Ids |
|---|---|---|
| 3 | Reduces the mana cost of all spells by 3% | **90363** ✅ |
| 5 | Cancelling a cast after at least 1 second of casting makes your next spell within 4 seconds cast 10% faster | **90364** + `90393` ⚠ |
| 8 | When an enemy uses an interrupt on you while you aren't casting, your next spell within 5 seconds casts 25% faster | **90365** + `90394` ⚠ |

> regular mage stats

*The 3pc uses aura **72** `MOD_POWER_COST_SCHOOL_PCT` (misc 127), which writes
`UNIT_FIELD_POWER_COST_MULTIPLIER` — a unit field the client reads for its own
cost check, so the saving shows client‑side. Same mechanism as Boon of Clarity.*

---

## Blocking everything

**There is no T2 gear.** `ItemSet.dbc` stops at 1054 (the last T1 set) and
`custom_hidden_itemset_bonus` holds 5 rows, none of them T2. Every bonus above is
tested by applying its carrier by hand. Delivery needs either ItemSet.dbc rows
(what T1 sets 1045/1052 use, and what `90329` *requires* — its `ShapeshiftMask` is
only honoured through `ItemSetEff`) or rows in `custom_hidden_itemset_bonus`.
