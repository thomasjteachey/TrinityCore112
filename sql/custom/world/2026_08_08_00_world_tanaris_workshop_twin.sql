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

-- The GO shell: plain GENERIC object, its collision comes from the M2's own
-- collision mesh (client-side).
DELETE FROM gameobject_template WHERE entry = 900001;
INSERT INTO gameobject_template (entry, type, displayId, name, size, Data0, Data1)
VALUES (900001, 5, 11000, 'Goblin Workshop (BG shell)', 1, 0, 0);

-- Reach must cover the building's footprint: the walls sit ~30 yards from
-- center, so with CombatReach 45 melee connects from just outside them
-- rather than only at the direct center.
DELETE FROM creature_model_info WHERE DisplayID = 40000;
INSERT INTO creature_model_info (DisplayID, BoundingRadius, CombatReach, Gender, DisplayID_Other_Gender)
VALUES (40000, 28, 45, 2, 0);

-- The attackable body. PassiveAI rather than UNIT_FLAG_PACIFIED: the core
-- strips PACIFIED from templates at load as a disallowed flag, while
-- PassiveAI legitimately never engages or retaliates. Scale 1.02 so the
-- creature's shell wraps the GO's mesh without z-fighting when co-located.
DELETE FROM creature_template WHERE entry = 900116;
INSERT INTO creature_template
  (entry, modelid1, name, subname, minlevel, maxlevel, faction, npcflag,
   unit_class, unit_flags, flags_extra, MovementType, RegenHealth,
   mechanic_immune_mask, HealthModifier, ArmorModifier, scale, speed_walk, speed_run,
   AIName, ScriptName)
VALUES
  (900116, 40000, 'Goblin Workshop', 'Converted WMO twin', 80, 80, 14, 0,
   1, 0, 1073742080, 0, 0, 551238167, 30, 1, 1.02, 1, 1.14286, 'PassiveAI', '');
