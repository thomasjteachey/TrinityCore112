# Playerbot Spell/Range Decision Tree Discrepancy Analysis

This compares:

- **Reference module**: `playerbot reference/mod-playerbots-master`
- **Active module in this repo build**: `src/server/scripts/Playerbot/Pvp`

Focus is specifically on **spell decision trees** and **range decision trees**.

## 1) Decision architecture mismatch: graph-driven vs single-pass selector

### Reference module
The reference playerbot uses a **trigger + action graph** with per-class strategy stacks.

- Class strategies register many `TriggerNode` entries with weighted `NextAction` outputs (example: Hunter strategy). The same class can queue different actions based on many independent triggers in one update cycle.
- Action nodes define prerequisites/alternatives/continuers, so the bot can fall back to second/third choices when an action is unavailable.

### Active module
The active module uses a **single sequential selector** per class:

- `BuildClassSpellContext` computes one `SpellDecision` and one target mode/guid per update.
- Each class function (`SelectHunterSpell`, `SelectMageSpell`, etc.) is an `if` chain with early returns.

### Practical discrepancy
The reference tree can express concurrent trigger pressure and fallback layering; the active tree is winner-takes-first-match. This can produce behavior drift where lower-priority but context-critical reactions in reference never execute if an earlier condition is true in active.

## 2) Range model mismatch: configurable strategy ranges vs hardcoded per-rule distances

### Reference module
Range behavior is built around configurable range categories and strategy transitions:

- `GetRange("spell"|"shoot"|"heal"|"melee"...)` resolves from AI value context or `PlayerbotAIConfig` defaults.
- Hunters switch between ranged/close strategies using explicit melee boundary logic (`<= 8.0f` and `> 8.0f`) via `SwitchToMeleeTrigger`/`SwitchToRangedTrigger` and corresponding combat actions.
- Generic combat strategy includes `reach spell` so movement/range correction is part of the decision graph, not only cast failure handling.

### Active module
Range is mostly encoded directly in selection rules and cast prechecks:

- Class logic uses many hardcoded thresholds (5, 8, 10, 15, 20, 30, 35, 40 yards, etc.).
- Cast executor checks `SpellInfo` max/min range and then issues `MoveFollow(target, maxRange - 1)` when out of range.

### Practical discrepancy
Reference range behavior is systemic/config-driven and strategy-based; active behavior is rule-local and spell-attempt-based. This can change when and how the bot repositions, especially for stance/range classes (Hunter, Mage, Shaman kiting profiles).

## 3) Targeting breadth mismatch: value-context target families vs narrow explicit selectors

### Reference module
Reference actions/strategies commonly pull from AI values such as current target, attacker sets, party members, CC candidates, and contextual values (`distance`, `attacker count`, cure targets, etc.). This supports broad target-family decisions.

### Active module
Targeting is narrower and more explicit in `PvpCore`:

- Enemy target usually resolves through a compact priority (`victim` -> selected unit -> closest enemy).
- Ally targeting is mostly selected-unit driven (plus specific helper scans for some spells).
- One resolved target mode/guid is passed into execution for that tick.

### Practical discrepancy
Reference can pivot across multiple target sets in the same reasoning pass; active often commits to one selected target lane. This can reduce opportunistic cross-target control/utility casts compared to reference.

## 4) Out-of-combat utility precedence mismatch

### Reference module
Non-combat utility exists, but combat strategy and trigger graph generally mediate transitions through strategy states.

### Active module
Every class selector first checks `SelectOutOfCombatEatDrinkOrMountSpell` and returns immediately when it emits a spell.

- This utility layer includes eat/drink/mount logic with a single nearby-hostile suppression boundary.
- It runs before class-specific combat spell checks in each class selector.

### Practical discrepancy
The active module has a centralized utility gate that can suppress class-tree evaluation more aggressively than reference’s distributed strategy/trigger handling, especially around edge transitions (post-combat, preparation, moving contact situations).

## 5) Cooldown/state memory mismatch in control spell handling

### Reference module
Cooldown and debuff cadence is generally integrated via action validity checks and existing aura/cast constraints inside the generic AI framework.

### Active module
The module introduces additional explicit state memory in class execution (e.g., custom warlock curse target cooldown map) and several spell-specific post-cast adjustments.

### Practical discrepancy
For some spells, active behavior has bespoke anti-recast throttles and post-cast bookkeeping that do not map 1:1 to reference trigger cadence, so cast timing can diverge even when both appear to choose the same spell family.

## 6) Execution pipeline mismatch: action framework vs direct cast transaction

### Reference module
`CastSpellAction` delegates viability and execution to the AI framework (`CanCastSpell`, action usefulness/possibility checks, strategy fallback).

### Active module
`PvpClassActions::Execute` is a direct transaction with detailed preflight gates:

- target validity by target mode,
- LOS/range checks,
- movement stopping for cast-time spells,
- facing corrections,
- mount teardown for non-mount casts,
- custom handling for special spell IDs.

### Practical discrepancy
Reference failures typically resolve by graph fallback on later actions; active failures return a direct reason for that one selected spell. If the chosen spell fails, there is no same-tick alternative action chain.

---

## High-impact discrepancy summary (most likely to be felt in PvP)

1. **No strategy-level melee/ranged mode machine in active module** (especially visible for Hunter).  
2. **Single-shot class selection** instead of multi-trigger weighted action graph.  
3. **Rule-local hardcoded distances** instead of mostly config/value-driven ranges.  
4. **Centralized out-of-combat gate** preceding class combat trees.  
5. **Different cast-failure recovery model** (single decision fail vs graph fallback).

These five differences explain most practical divergence in both spell choice and repositioning behavior.
