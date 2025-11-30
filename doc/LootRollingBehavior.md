# Loot rolling behavior

When loot is generated for a group, the server applies the party's loot method and threshold to decide whether players roll on an item or receive it without a roll:

- **Group Loot** and **Need Before Greed** only start a roll when the item quality meets or exceeds the group's loot threshold (or when the item is account-bound). Items below the threshold are flagged as under-threshold and are handed out via round-robin instead of triggering a roll prompt.
- Items that do not meet the threshold are still subject to the group's ownership rules (round-robin owner or released loot) but do not show a roll window because they are explicitly marked as under-threshold.

If you see some drops triggering rolls while others do not, check the group's loot threshold. Raising the threshold will make more items require rolls; lowering it will allow more items to be distributed automatically.
