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
--     display rows must be minted into patch-F's copies and shipped in the
--     ART patch, or the client never sees them and renders checkered cubes.
--     GameObjectDisplayInfo is not in patch-F, so GO rows ride enUS-8 fine.
--   * The .m2/.skin must be in a non-locale patch.
--   * That art patch is **patch-Y** since the 2026-08-08 split (production
--     patch-Z was emptied to a forge-delta stub; anything left only in a
--     hand-built local patch-Z is erased by the next launcher run).
--
-- Replayable, and the migration source of truth for other realms.

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
-- stripped from templates at load; plain PassiveAI never ends the fight.)
-- The immunity mask deliberately EXCLUDES the charm-mechanic bit (551238166,
-- not ...167): a garrisonable building is boarded via a charm-class aura, so
-- charm immunity and the vehicle system are mutually exclusive. Scale is
-- EXACTLY 1.0: oversized wraps (1.005 - 1.02) were tried to dodge z-fighting
-- against the shell, but the shell became invisible (collision-only model)
-- so there is nothing to z-fight and any wrap would only drift the visible
-- surface off the collision mesh.
-- Every column is spelled out because this row is the migration source of
-- truth for other realms: the belt-and-braces immobility values are easy to
-- lose in a partial insert. unit_flags2 2048 = UNIT_FLAG2_DISABLE_TURN and
-- speed_walk/speed_run 0.00001 are the last two of those belts (the seat
-- itself is what actually makes movement impossible - see below); RegenHealth
-- 0 keeps a damaged building damaged; flags_extra 1073742080 = no XP + civilian
-- ...ish combination already proven in the dev row.
DELETE FROM creature_template WHERE entry = 900116;
INSERT INTO creature_template
  (entry, difficulty_entry_1, difficulty_entry_2, difficulty_entry_3, KillCredit1, KillCredit2,
   modelid1, modelid2, modelid3, modelid4, name, subname, IconName, gossip_menu_id,
   minlevel, maxlevel, exp, faction, npcflag, speed_walk, speed_run, scale, rank,
   dmgschool, BaseAttackTime, RangeAttackTime, BaseVariance, RangeVariance, unit_class,
   unit_flags, unit_flags2, dynamicflags, family, type, type_flags, lootid, pickpocketloot,
   skinloot, PetSpellDataId, VehicleId, mingold, maxgold, AIName, MovementType, HoverHeight,
   HealthModifier, ManaModifier, ArmorModifier, DamageModifier, ExperienceModifier,
   RacialLeader, movementId, RegenHealth, mechanic_immune_mask, spell_school_immune_mask,
   flags_extra, ScriptName, StringId, VerifiedBuild)
VALUES
  -- faction 35 (friendly-to-all) is the boarding-test state; the RTS gives
  -- buildings per-team factions and this row follows ownership at runtime.
  -- npcflag 16777216 = SPELLCLICK (right-click boards). VehicleId 1000 is a
  -- Vehicle.dbc byte-clone of Stampy's kit 121 (its plain seat profile was
  -- the only one of four whose exits didn't hang the client's control
  -- handback) whose single seat is now the custom seat 90000: seat 1705
  -- minus CAN_CONTROL, keeping CAN_CAST + CAN_ENTER_OR_EXIT. The garrisoned
  -- rider is therefore a PASSENGER with the vehicle's action bar (the stock
  -- gunner arrangement, wired server-side in Vehicle.cpp/PetHandler.cpp):
  -- the client never movers the building, so steering, turning and jumping
  -- are impossible by architecture - EXACT zero, which no possess-seat
  -- mechanism (facing limits, TurnSpeed, turn rate) ever delivered.
  (900116, 0, 0, 0, 0, 0,
   40000, 0, 0, 0, 'Goblin Workshop', 'Converted WMO twin', NULL, 0,
   60, 60, 0, 35, 16777216, 0.00001, 0.00001, 1, 0,
   0, 0, 0, 1, 1, 1,
   0, 2048, 0, 0, 1, 0, 0, 0,
   0, 0, 1000, 0, 0, '', 0, 1,
   30, 1, 1, 1, 1,
   0, 0, 0, 551238166, 0,
   1073742080, 'npc_rts_building', NULL, NULL);

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

-- Belt on top of the non-control seat: permanent MOD_ROOT aura 42716 "Self
-- Root Forever (No Visual)" pins the creature against anything server-side
-- that might ever try to move it. NOT a stun (9454 was tried first): the
-- client refuses the Leave Vehicle action while its mover is stunned - the
-- button simply did nothing - and a stunned vehicle cannot cast its action
-- bar. Root gates neither, so future custom building spells do NOT need
-- SPELL_ATTR5_USABLE_WHILE_STUNNED. Being a genuine MOD_ROOT aura, 42716
-- also keeps Unit::SetControlled(false, UNIT_STATE_ROOT) from ever unrooting
-- the creature when an enemy's temporary root expires.
DELETE FROM creature_template_addon WHERE entry = 900116;
INSERT INTO creature_template_addon (entry, auras) VALUES (900116, '42716');

-- Dismount just outside the gate (user-picked spot ~23 yd out along the
-- door side); offset mode (ExitParamValue 1) is relative to the vehicle's
-- facing, and the exit spline adds the vehicle's CollisionHeight so the
-- passenger settles onto the ground. Seat 90000 is the building's own custom
-- seat (minted by tools/tanaris/seat_tool.py), so this no longer touches
-- Stampy's stock seat 1705 as an earlier revision did.
--
-- WHERE THE RIDER SITS is NOT here, and NOT in VehicleSeat.dbc either: it is
-- M2 attachment 0 inside World\TanarisBG\WgWorkshopBG.m2, at model-space
-- (0.4815, -2.2578, 32.5056) - the TOWER TOP - written by
-- tools/tanaris/add_m2_attachment.py. The client takes the seat position
-- from the attachment named by VehicleSeat.AttachmentID and treats
-- AttachmentOffset only as a FALLBACK for when that attachment cannot be
-- resolved, so seat 90000 keeps AttachmentID 0 with offset (0,0,0) exactly
-- like the stock seats (Stampy's 1705 is AttachmentID 21, offset zero, and
-- its rider still sits on the mammoth's back). Consequence, also stock: the
-- SERVER derives the passenger's transport offset from AttachmentOffset
-- alone, so it treats a garrisoned rider as standing at the building's
-- origin while the client draws them on the tower.
DELETE FROM vehicle_seat_addon WHERE SeatEntry IN (1705, 90000);
INSERT INTO vehicle_seat_addon (SeatEntry, SeatOrientation, ExitParamX, ExitParamY, ExitParamZ, ExitParamO, ExitParamValue)
VALUES (90000, 0.1470, 21.8570, -7.2697, 1, 0.0449, 1);
