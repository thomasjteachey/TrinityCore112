-- Allow Stockades PvPvE template to accept solo entries for testing
UPDATE `pvpve_dungeon_template`
SET `MinPlayersPerTeam` = 1,
    `MaxPlayersPerTeam` = 2
WHERE `Id` = 1;
