-- Keep custom polearm/staff inner auras active across death and lifecycle transitions.
UPDATE `spell_dbc`
SET `Attributes` = `Attributes` | 64,
    `AttributesEx3` = `AttributesEx3` | 1048576
WHERE `Id` IN (89769, 89770, 89771, 89772, 89773);
