# centurion-bundle

How `centurion/` is refreshed from the live realm. Run on the realm's server.

- `build.py <out>` writes `world/`, `characters/` and `auth/` for `centurion/sql/` from
  the live databases (read-only). The table lists say which tables go in: backup and
  scratch tables stay out, and a new table the server reads has to be added by hand.
  `realm_block.sql` is the localhost realm row appended to `auth_data.sql`.
- Test it before committing: `MYSQL="mysql ..." AUTH_DB=x WORLD_DB=y CHAR_DB=z
  ../../centurion/sql/import.sh` from the output folder, into scratch databases, then
  check for orphaned items and pets and drop them.
- DBCs: take the server's copy only when it holds every record id the repository's copy
  has. Otherwise merge the repository-only records into the server's copy; some rows
  exist only here.
- Patches: every non-optional `FileMap` entry for `centurion` in the launcher's
  `src/common/constants.ts`, copied from the download server with their `.version`
  files, zips over 50 MiB split into 49 MiB `.partNN` pieces, and `patches.md5`
  rewritten. `centurion/patches/join.sh` must pass afterwards.
- `centurion/conf/worldserver.conf` is the live file with the database strings, folders,
  SOAP, realm id, port and GM Panel realms put back to the `.dist` values. Check that no
  live password remains in it before committing.
- `centurion/mmaps/1690*` is the live server's Summoner's Rift navmesh.
