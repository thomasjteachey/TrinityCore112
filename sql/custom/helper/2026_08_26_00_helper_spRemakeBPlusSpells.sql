DROP PROCEDURE IF EXISTS helper.spRemakeBPlusSpells;
DELIMITER $$
CREATE PROCEDURE helper.spRemakeBPlusSpells()
COMMENT 'Rebuild dbc.spell_bplus from dbc.spell_lplus, keeping B+-only data and applying B+ overrides'
BEGIN
    /*
      Barracks+ runs Legionnaire+'s spell content, so spell_bplus is derived from
      spell_lplus rather than maintained by hand. Two classes of B+ data must
      SURVIVE that rebuild, and both have been lost to a careless refresh before:

        1. REAGENTS / TOTEMS. L+ strips every reagent (spell_lplus has 0 rows with
           Reagent_1); B+ is a PvE realm and keeps them (~1963 rows). A plain
           "INSERT ... SELECT * FROM spell_lplus" silently wipes them, which is
           exactly how B+ lost its reagents on 2026-07-06.
        2. CLASSIC TELEPORT NAMES. L+ repurposed the classic teleport ids for its
           own destinations (3567 became Gurubashi, 3562 Caverns Waste of Time,
           etc). B+ keeps the classic names.

      Everything else is taken from L+ verbatim.

      NOTE: this rebuilds the MIRROR only. It does not touch the binary Spell.dbc.
      Do NOT run dbcgen for B+ to regenerate it unless you have confirmed dbcgen
      emits reagents - as of 2026-08-26 it does not (no 'reagent' anywhere in
      dbcgen.py), which is the original cause of the reagent loss.
    */
    DECLARE v_before INT DEFAULT 0;
    DECLARE v_after  INT DEFAULT 0;
    DECLARE v_reag   INT DEFAULT 0;

    SELECT COUNT(*) INTO v_before FROM dbc.spell_bplus;

    -- full snapshot, overwritten each run
    DROP TABLE IF EXISTS dbc.spell_bplus_prev_remake;
    CREATE TABLE dbc.spell_bplus_prev_remake LIKE dbc.spell_bplus;
    INSERT INTO dbc.spell_bplus_prev_remake SELECT * FROM dbc.spell_bplus;

    -- keep B+'s materials
    DROP TEMPORARY TABLE IF EXISTS tmp_keep_materials;
    CREATE TEMPORARY TABLE tmp_keep_materials AS
    SELECT ID, Totem_1, Totem_2,
           Reagent_1, Reagent_2, Reagent_3, Reagent_4,
           Reagent_5, Reagent_6, Reagent_7, Reagent_8,
           ReagentCount_1, ReagentCount_2, ReagentCount_3, ReagentCount_4,
           ReagentCount_5, ReagentCount_6, ReagentCount_7, ReagentCount_8
    FROM dbc.spell_bplus
    WHERE Reagent_1 <> 0 OR Reagent_2 <> 0 OR Reagent_3 <> 0 OR Reagent_4 <> 0
       OR Reagent_5 <> 0 OR Reagent_6 <> 0 OR Reagent_7 <> 0 OR Reagent_8 <> 0
       OR Totem_1 <> 0 OR Totem_2 <> 0;
    CREATE INDEX ix_keep_mat ON tmp_keep_materials (ID);

    -- rebuild from L+
    TRUNCATE TABLE dbc.spell_bplus;
    INSERT INTO dbc.spell_bplus SELECT * FROM dbc.spell_lplus;

    -- restore B+'s materials
    UPDATE dbc.spell_bplus b
      JOIN tmp_keep_materials k ON k.ID = b.ID
       SET b.Totem_1 = k.Totem_1, b.Totem_2 = k.Totem_2,
           b.Reagent_1 = k.Reagent_1, b.Reagent_2 = k.Reagent_2,
           b.Reagent_3 = k.Reagent_3, b.Reagent_4 = k.Reagent_4,
           b.Reagent_5 = k.Reagent_5, b.Reagent_6 = k.Reagent_6,
           b.Reagent_7 = k.Reagent_7, b.Reagent_8 = k.Reagent_8,
           b.ReagentCount_1 = k.ReagentCount_1, b.ReagentCount_2 = k.ReagentCount_2,
           b.ReagentCount_3 = k.ReagentCount_3, b.ReagentCount_4 = k.ReagentCount_4,
           b.ReagentCount_5 = k.ReagentCount_5, b.ReagentCount_6 = k.ReagentCount_6,
           b.ReagentCount_7 = k.ReagentCount_7, b.ReagentCount_8 = k.ReagentCount_8;

    -- Warlock soul shards stay free on B+ (owner decision, 2026-08-25)
    UPDATE dbc.spell_bplus
       SET Reagent_1=0, Reagent_2=0, Reagent_3=0, Reagent_4=0,
           Reagent_5=0, Reagent_6=0, Reagent_7=0, Reagent_8=0,
           ReagentCount_1=0, ReagentCount_2=0, ReagentCount_3=0, ReagentCount_4=0,
           ReagentCount_5=0, ReagentCount_6=0, ReagentCount_7=0, ReagentCount_8=0
     WHERE 6265 IN (Reagent_1,Reagent_2,Reagent_3,Reagent_4,Reagent_5,Reagent_6,Reagent_7,Reagent_8);

    -- B+ OVERRIDE: classic teleport names. 3581/3577/3578/3579/3580/665 are the
    -- LEARN_SPELL teachers; the lower ids are the teleports they teach.
    UPDATE dbc.spell_bplus SET Name_Lang_enUS = 'Teleport: Ironforge'     WHERE ID IN (3562, 3581);
    UPDATE dbc.spell_bplus SET Name_Lang_enUS = 'Teleport: Undercity'     WHERE ID IN (3563, 3577);
    UPDATE dbc.spell_bplus SET Name_Lang_enUS = 'Teleport: Darnassus'     WHERE ID IN (3565, 3578);
    UPDATE dbc.spell_bplus SET Name_Lang_enUS = 'Teleport: Orgrimmar'     WHERE ID IN (3567, 3580);
    UPDATE dbc.spell_bplus SET Name_Lang_enUS = 'Teleport: Thunder Bluff' WHERE ID IN (3566, 3579);
    UPDATE dbc.spell_bplus SET Name_Lang_enUS = 'Teleport: Stormwind'     WHERE ID IN (3561, 665);

    -- B+ OVERRIDE: classic teleport destinations (world DB, not the DBC).
    -- 3565 and 3567 were genuinely relocated by L+, not just renamed.
    UPDATE bplusworld.spell_target_position
       SET MapID=1, PositionX=9656.54, PositionY=2518.26, PositionZ=1331.66, Orientation=0
     WHERE ID=3565 AND EffectIndex=0;
    UPDATE bplusworld.spell_target_position
       SET MapID=1, PositionX=1469.96, PositionY=-4222.51, PositionZ=58.9938, Orientation=0
     WHERE ID=3567 AND EffectIndex=0;

    SELECT COUNT(*) INTO v_after FROM dbc.spell_bplus;
    SELECT COUNT(*) INTO v_reag  FROM dbc.spell_bplus WHERE Reagent_1 <> 0;

    SELECT v_before AS rows_before, v_after AS rows_after, v_reag AS spells_with_reagents,
           (SELECT COUNT(*) FROM dbc.spell_bplus
             WHERE 6265 IN (Reagent_1,Reagent_2,Reagent_3,Reagent_4,Reagent_5,Reagent_6,Reagent_7,Reagent_8))
             AS soulshard_spells_should_be_0,
           (SELECT GROUP_CONCAT(CONCAT(ID,'=',Name_Lang_enUS) ORDER BY ID SEPARATOR ' | ')
              FROM dbc.spell_bplus WHERE ID IN (3562,3563,3565,3566,3567,3561)) AS classic_teleports;

    DROP TEMPORARY TABLE IF EXISTS tmp_keep_materials;
END$$
DELIMITER ;
