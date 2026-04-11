# Playerbot Decision Tree Discrepancy Report (Reference vs Active Module)

## Scope
- **Reference analyzed:** `playerbot reference/mod-playerbots-master` (AzerothCore playerbot framework).
- **Active module analyzed:** `src/server/scripts/Playerbot/Pvp` (current Trinity module in this repo).
- Focus: **spell-decision logic** and **range-decision logic**.

## Key Logical Discrepancies

1. **Decision architecture differs (trigger graph vs single-pass priority list).**
   - Reference module uses trigger/action graph composition (`TriggerNode`, `NextAction`) where many rules can activate and carry prerequisites/alternatives.
   - Active module performs a per-tick class spell selection by building one candidate list and taking the highest priority result.
   - Impact: reference supports richer layered behavior chains; active module is deterministic single-winner each tick.

2. **Active module hard-disables class spell automation during active battleground combat.**
   - In active module `BuildClassSpellContext`, class spells are explicitly skipped when battleground state is active.
   - Reference module has no equivalent “BG-active disable” gate in the core combat strategy path and continues trigger evaluation in combat.
   - Impact: in battleground fights, active module can drop to non-class automation behavior while reference keeps full combat spell trees online.

3. **Range control in reference is proactive trigger-based; active is mostly reactive cast-failure-based.**
   - Reference has explicit triggers like `enemy out of spell -> reach spell` and `enemy too close for spell -> flee`.
   - Active module validates LOS/range at cast time; on out-of-range it issues follow movement and returns failure, while `too_close` returns failure without an explicit retreat primitive in this layer.
   - Impact: reference continuously maintains combat spacing via movement actions; active module often discovers range problems only when a cast attempt is evaluated.

4. **Fallback depth differs (graph alternatives/prerequisites vs single suppressed retry).**
   - Reference action nodes can define alternatives and prerequisites (e.g., fallback actions from the same node).
   - Active module attempts one fallback by suppressing the initially selected spell and re-running selection once.
   - Impact: reference can chain multiple recovery paths naturally; active module has shallower fallback search and may stall/repeat in tight edge cases.

5. **Target model granularity differs.**
   - Reference selection is heavily value-driven by target roles (`enemy healer target`, `cc target`, `party member to heal`, etc.) and class strategy context.
   - Active module centers on a selected enemy target + optional selected ally target and class-specific helper selectors.
   - Impact: reference better expresses role/encounter-specific target switching in the decision tree itself; active tends to be more compact and centralized.

6. **Spec/profile richness differs.**
   - Reference classes are split into many strategy families (e.g., mage arcane/fire/frost strategies, dedicated boost/cc/aoe strategies).
   - Active module uses a compact “classic profile” inference and one class selector path.
   - Impact: active logic is easier to reason about but less behaviorally granular than the reference strategy matrix.

7. **Healer range correction is explicit in reference strategy trees; active relies on spell-range gate and target selection distance checks.**
   - Reference healer strategies include direct trigger `party member to heal out of spell range -> reach party member to heal`.
   - Active module enforces ally selection distance and cast range checks, but does not model a dedicated healer-range trigger tree in the same way.
   - Impact: reference healer movement intent is explicit and reusable as behavior nodes; active healer range behavior is implicit through validation + follow movement on failed cast.

## Bottom Line
The active Trinity module appears intentionally simplified and safety-gated compared with the reference module’s broad trigger graph. The largest behavioral divergence affecting live PvP combat is the **active-battleground class-spell disable gate**, followed by **reactive (cast-time) range correction** replacing **proactive trigger-based spacing**.
