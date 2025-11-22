# Stockades PvPvE design notes

- The dungeon uses three distinct spawn points and a locked boss room.
- Each team locks its chosen spawn when entering; the spawn cannot be reused in the same instance.
- After all three spawns are taken, the instance is sealed and no additional teams can zone in, even if others leave.
- When a team enters, every player on that team is marked as unable to return to that specific instance. Once they leave or are eliminated, queueing will direct them to another instance (or spawn a fresh one).
