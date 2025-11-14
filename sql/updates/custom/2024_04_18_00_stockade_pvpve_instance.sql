-- Ensure the Stockades instance uses the PvPvE-aware script
UPDATE `instance_template` SET `script`='instance_the_stockade_pvpve' WHERE `map`=34;
