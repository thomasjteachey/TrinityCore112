-- Align Stockades PvPvE template with 2-player queue requirements
UPDATE `pvpve_dungeon_template`
SET `MinPlayersPerTeam` = 2,
    `MaxPlayersPerTeam` = 2
WHERE `Id` = 1;
