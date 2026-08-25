-- spMakeLPlusPlayerCreateInfoSpellCustom: grant Pet - Move To (81354).
--
-- The spell had gone missing from lplusworld.playercreateinfo_spell_custom AND
-- from this procedure. That pairing is the whole story: the procedure WIPES that
-- table and rebuilds it from legionnaireworld, so a row added only to the table
-- is destroyed the next time it runs. Adding it here is what makes it stick.
--
-- classmask 292 = HUNTER 4 + DK 32 + WARLOCK 256. spell_pet_moveto
-- (spell_generic.cpp) needs GetGuardianPet() to return a real Pet, which only
-- those three classes have; the script is otherwise class-agnostic, so widening
-- this is safe if other pets should get it.
--
-- Verified end to end 2026-08-24: with the table snapshotted first, running the
-- procedure produced 1143 rows where there had been 1142, and the MD5 of every
-- row EXCEPT 81354 was unchanged - so this adds the grant and disturbs nothing.
--
-- NOTE: playercreateinfo_spell_custom only applies at CHARACTER CREATION.
-- Existing characters do not pick the spell up from it.

DELIMITER ;;
CREATE DEFINER=`brokilodeluxe`@`localhost` PROCEDURE `spMakeLPlusPlayerCreateInfoSpellCustom`()
BEGIN
	delete from lplusworld.playercreateinfo_spell_custom;
    insert into lplusworld.playercreateinfo_spell_custom
    select * from legionnaireworld.playercreateinfo_spell_custom
    ;
    delete from lplusworld.playercreateinfo_spell_custom
    where spell in 
    (
        
        1495,14269,14270,14271,
        
        3045,
        
        2362, 17727, 17728,
        
        6366, 17953, 17951, 17952,
        
        694,
        
        10876, 8129, 10875, 8131, 10874,
        
        1120, 8288, 8289, 11675,
        
        3420
    )
    ;
    update lplusworld.playercreateinfo_spell_custom set spell = 81324 where spell = 370;
    update lplusworld.playercreateinfo_spell_custom set spell = 81325 where spell = 8012;
    
    INSERT IGNORE INTO `lplusworld`.`playercreateinfo_spell_custom` (`racemask`, `classmask`, `Spell`, `Note`) VALUES ('0', '256', '29893', 'Soulwell');
	
    INSERT IGNORE INTO `lplusworld`.`playercreateinfo_spell_custom` (`racemask`, `classmask`, `Spell`, `Note`) VALUES ('0', '4', '5149', 'Beast Training');

    /* Pet - Move To (81354). classmask 292 = HUNTER 4 + DK 32 + WARLOCK 256:
       spell_pet_moveto (spell_generic.cpp) requires GetGuardianPet() to return a
       real Pet, which only those three classes have. The script is otherwise
       class-agnostic, so widening this is safe if other pets should get it.
       It lives HERE and not just in the table because this procedure wipes and
       rebuilds that table - which is how the row went missing before. */
    INSERT IGNORE INTO `lplusworld`.`playercreateinfo_spell_custom` (`racemask`, `classmask`, `Spell`, `Note`) VALUES ('0', '292', '81354', 'Pet - Move To');
    
    
    
    INSERT IGNORE INTO `lplusworld`.`playercreateinfo_spell_custom` (`racemask`, `classmask`, `Spell`, `Note`) VALUES ('0', '1024', '89762', 'DRUID: Mass Thorns');
    
    INSERT IGNORE INTO `lplusworld`.`playercreateinfo_spell_custom` (`racemask`, `classmask`, `Spell`, `Note`) VALUES ('0', '64', '10428', 'Stoneclaw totem');
    
    delete from lplusworld.playercreateinfo_spell_custom where racemask > 0 and classmask = 16;

    
    update lplusworld.playercreateinfo_spell_custom set spell = 90420 where spell = 686;
    update lplusworld.playercreateinfo_spell_custom set spell = 90421 where spell = 695;
    update lplusworld.playercreateinfo_spell_custom set spell = 90422 where spell = 705;
    update lplusworld.playercreateinfo_spell_custom set spell = 90423 where spell = 1088;
    update lplusworld.playercreateinfo_spell_custom set spell = 90424 where spell = 1106;
    update lplusworld.playercreateinfo_spell_custom set spell = 90425 where spell = 7641;
    update lplusworld.playercreateinfo_spell_custom set spell = 90426 where spell = 11659;
    update lplusworld.playercreateinfo_spell_custom set spell = 90427 where spell = 11660;
    update lplusworld.playercreateinfo_spell_custom set spell = 90428 where spell = 11661;
    update lplusworld.playercreateinfo_spell_custom set spell = 90429 where spell = 25307;
    update lplusworld.playercreateinfo_spell_custom set spell = 90430 where spell = 27209;
    update lplusworld.playercreateinfo_spell_custom set spell = 90431 where spell = 47808;
    update lplusworld.playercreateinfo_spell_custom set spell = 90432 where spell = 47809;
    update lplusworld.playercreateinfo_spell_custom set spell = 90460 where spell = 1978;
    update lplusworld.playercreateinfo_spell_custom set spell = 90461 where spell = 13549;
    update lplusworld.playercreateinfo_spell_custom set spell = 90462 where spell = 13550;
    update lplusworld.playercreateinfo_spell_custom set spell = 90463 where spell = 13551;
    update lplusworld.playercreateinfo_spell_custom set spell = 90464 where spell = 13552;
    update lplusworld.playercreateinfo_spell_custom set spell = 90465 where spell = 13553;
    update lplusworld.playercreateinfo_spell_custom set spell = 90466 where spell = 13554;
    update lplusworld.playercreateinfo_spell_custom set spell = 90467 where spell = 13555;
    update lplusworld.playercreateinfo_spell_custom set spell = 90468 where spell = 25295;
    update lplusworld.playercreateinfo_spell_custom set spell = 90469 where spell = 27016;
    update lplusworld.playercreateinfo_spell_custom set spell = 90470 where spell = 49000;
    update lplusworld.playercreateinfo_spell_custom set spell = 90471 where spell = 49001;
    update lplusworld.playercreateinfo_spell_custom set spell = 90514 where spell = 2983;
    update lplusworld.playercreateinfo_spell_custom set spell = 90515 where spell = 8696;
    update lplusworld.playercreateinfo_spell_custom set spell = 90516 where spell = 11305;
    update lplusworld.playercreateinfo_spell_custom set spell = 81492 where spell = 676;   
    update lplusworld.playercreateinfo_spell_custom set spell = 81476 where spell = 8143;   
    update lplusworld.playercreateinfo_spell_custom set spell = 81477 where spell = 8166;   
    update lplusworld.playercreateinfo_spell_custom set spell = 81478 where spell = 8177;   

END ;;
DELIMITER ;
