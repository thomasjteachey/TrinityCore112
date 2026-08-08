-- The converted Goblin Workshop as a twin-pattern building: a GO shell for
-- collision plus a creature wearing the same model as the attackable body.
--
-- The model is World\TanarisBG\WgWorkshopBG.m2, converted from the Wintergrasp
-- WMO by tools/tanaris/wmo2m2.py, registered as GO display 11000 and creature
-- display 40000 (CreatureModelData 4000) by mint_building_display.py.
--
-- CLIENT-SIDE PREREQUISITES, learned the hard way:
--   * patch-F ships its own CreatureDisplayInfo/CreatureModelData (the HD
--     creature pack, 145 extra models) and OUTRANKS patch-enUS-8. Creature
--     display rows must be minted into patch-F's copies and shipped in
--     patch-Z, or the client never sees them and renders checkered cubes.
--     GameObjectDisplayInfo is not in patch-F, so GO rows ride enUS-8 fine.
--   * The .m2/.skin must be in a non-locale patch (patch-Z).
--
-- Replayable.

-- The GO shell: INVISIBLE collision-only model (display 11001, built by
-- wmo2m2.py --collision-only, same shape as Blizzard's FakeCollision doors:
-- zero render geometry, full collision mesh). The creature twin carries every
-- pixel at exact scale 1.0 with nothing to z-fight against. Display 11000 is
-- the same building WITH visuals, kept for GO-only uses.
DELETE FROM gameobject_template WHERE entry = 900001;
INSERT INTO gameobject_template (entry, type, displayId, name, size, Data0, Data1)
VALUES (900001, 5, 11001, 'Goblin Workshop (BG shell)', 1, 0, 0);

-- Reach must cover the building's footprint: the walls sit ~30 yards from
-- center, so with CombatReach 45 melee connects from just outside them
-- rather than only at the direct center.
DELETE FROM creature_model_info WHERE DisplayID = 40000;
INSERT INTO creature_model_info (DisplayID, BoundingRadius, CombatReach, Gender, DisplayID_Other_Gender)
VALUES (40000, 28, 45, 2, 0);

-- The attackable body, driven by npc_rts_building (Custom/rts_building.cpp):
-- never retaliates, and clears its own combat five seconds after the last hit
-- so attackers disengage the way they would from a player. (PACIFIED gets
-- stripped from templates at load; plain PassiveAI never ends the fight.) Scale 1.0001: a 1.02
-- wrap avoided z-fighting but shifted the visible surface off the GO's
-- collision mesh; near-exact scale keeps collision honest and the z-fighting
-- is accepted (user's call).
DELETE FROM creature_template WHERE entry = 900116;
INSERT INTO creature_template
  (entry, modelid1, name, subname, minlevel, maxlevel, faction, npcflag,
   unit_class, unit_flags, flags_extra, MovementType, RegenHealth,
   mechanic_immune_mask, HealthModifier, ArmorModifier, scale, speed_walk, speed_run,
   VehicleId, AIName, ScriptName)
VALUES
  -- faction 35 (friendly-to-all) is the boarding-test state; the RTS gives
  -- buildings per-team factions and this row follows ownership at runtime.
  -- npcflag 16777216 = SPELLCLICK (right-click boards). VehicleId 160 is the
  -- Antipersonnel Cannon kit: a ground turret with clean enter/exit (the
  -- bomber gun seat's exit profile, built for mid-flight ejection, left
  -- riders stuck "moving" over a stationary building). Seat CAN_ATTACK is
  -- irrelevant: vehicle-bar spells are cast by the vehicle itself, the flag
  -- only gates the rider's own spellbook. The rider CAN cosmetically spin the
  -- building until npc_rts_building's control revocation is built - the
  -- server never accepts the turn, so only the rider sees it.
  (900116, 40000, 'Goblin Workshop', 'Converted WMO twin', 60, 60, 35, 16777216,
   1, 0, 1073742080, 0, 0, 551238167, 30, 1, 1.0, 1, 1.14286,
   160, '', 'npc_rts_building');

-- The vehicle action bar shown to whoever garrisons the building. Index 0-7.
-- 51421 is the donor turret's Cannon Blast, a placeholder until the custom
-- building spells go through the spell_lplus pipeline.
DELETE FROM creature_template_spell WHERE CreatureID = 900116;
INSERT INTO creature_template_spell (CreatureID, `Index`, Spell) VALUES
  (900116, 0, 51421);

-- Right-click boarding, restricted to FRIENDLY units (user_type 1): an enemy
-- building is a target, your own is a garrison.
DELETE FROM npc_spellclick_spells WHERE npc_entry = 900116;
INSERT INTO npc_spellclick_spells (npc_entry, spell_id, cast_flags, user_type) VALUES
  (900116, 46598, 1, 1);

-- Dismount in front of the gate, not inside the building. Vehicle exits place
-- the passenger at the vehicle's origin - the dead center of a sixty-yard
-- building, wedged inside its own collision shell. Offset mode (ExitParamValue
-- 1) relocates the exit relative to the vehicle's facing; the door is baked
-- onto model +X, so +35 yards along facing lands just outside the gate (the
-- wall sits at +23.7). NOTE: seat 2029 is kit 160's stock seat, so this also
-- moves exits for real Antipersonnel Cannons - unused on this server, and a
-- dedicated seat row can replace this if that ever changes.
DELETE FROM vehicle_seat_addon WHERE SeatEntry = 2029;
INSERT INTO vehicle_seat_addon (SeatEntry, SeatOrientation, ExitParamX, ExitParamY, ExitParamZ, ExitParamO, ExitParamValue)
VALUES (2029, 0, 35, 0, 1, 0, 1);
