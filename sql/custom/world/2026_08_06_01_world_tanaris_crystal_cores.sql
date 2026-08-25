-- Example "building core" creatures for the Tanaris RTS layer.
--
-- The plan: a structure is a GameObject for the visuals and collision, with a
-- creature inside it as the thing you actually kill. Destructible GameObjects
-- can only be damaged by SPELL_EFFECT_GAMEOBJECT_DAMAGE (87), which is why a
-- siege vehicle is currently needed; a creature takes damage from every normal
-- ability, so the core makes structures killable by anyone.
--
-- These are the candidate models, one creature each, so they can be compared
-- side by side in-game:
--
--     .npc add 900100      (etc.)
--
-- Set up as a proper core rather than a plain dummy:
--   faction 14          hostile to everyone, so both teams can attack it
--   npc_rts_building    the RTS structure AI: never retaliates, and drops
--                       combat five seconds after the last hit so attackers
--                       disengage like they would from a player
--   MovementType 0      never wanders or chases
--   flags_extra         NO_TAUNT | IMMUNITY_KNOCKBACK, so it cannot be pulled
--                       out of its building
--   mechanic_immune_mask copied from Living Constellation (33052) - the full
--                       crowd-control immunity set
--   RegenHealth 0       damage sticks between tests instead of ticking back
--
-- Replayable.

DELETE FROM creature_template WHERE entry BETWEEN 900100 AND 900115;

INSERT INTO creature_template
  (entry, modelid1, name, subname, minlevel, maxlevel, faction, npcflag,
   unit_class, unit_flags, flags_extra, MovementType, RegenHealth,
   mechanic_immune_mask, HealthModifier, ArmorModifier, speed_walk, speed_run,
   AIName, ScriptName)
VALUES
-- The recommended one. Its environment map is SILITHUS_CRYSTAL_ENVIRONMENT_MAP,
-- so it is lit for desert and sits naturally in Tanaris sand.
  (900100, 17856, 'Crystal Core', 'Power Crystal - blue, desert-lit',        80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900101, 19258, 'Crystal Core', 'Power Crystal - 1.5x scale',              80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900102, 19257, 'Crystal Core', 'Power Crystal - naga glass reskin',       80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),

-- Machine rather than mineral - suits goblin engineering. Orb on a pedestal.
  (900103, 28741, 'Titan Core', 'Titan Orb - machine, on a base',            80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),

-- Arcane. The model is literally named Dalaran_BuildingCrystal_01.
  (900104, 28876, 'Arcane Core', 'Dalaran Building Crystal',                 80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),

-- The only pair with a purpose-made cracked variant: swap 900105 -> 900106 at
-- a health threshold and the core visibly breaks. Green/undead, so not neutral,
-- but it proves the damage-state pattern.
  (900105, 16135, 'Shard Core', 'Scourge Crystal - INTACT of pair',          80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900106, 16136, 'Shard Core', 'Scourge Crystal - DAMAGED of pair',         80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900107, 22506, 'Shard Core', 'Scourge Crystal 02',                        80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),

-- Natural crystal formations from Sholazar. Note the odd scales: the "large"
-- model ships at 0.6 and the small one at 2.5.
  (900108, 25931, 'Stone Core', 'Oracle Crystal - small model at 2.5x',      80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900109, 25929, 'Stone Core', 'Oracle Crystal - large model at 0.6x',      80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),

-- Other candidates worth eyeballing before committing.
  (900110, 22669, 'Fel Core', 'Demon Crystal 02',                            80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900111, 26620, 'Sc Core', 'Creature_Sc_Crystal',                          80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900112, 24813, 'Orb Core', 'Scrying Orb - floating',                      80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900113,  9832, 'Ziggurat Core', 'Ziggurat Crystal',                       80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900114, 11490, 'Portal Core', 'Crystal Portal - animated',                80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building'),
  (900115, 16034, 'Portal Core', 'Crystal Portal - 3x scale',                80, 80, 14, 0, 1, 131072, 1073742080, 0, 0, 551238167, 20, 1, 1, 1.14286, '', 'npc_rts_building');
