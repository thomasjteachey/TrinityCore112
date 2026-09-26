#!/usr/bin/env bash
# Loads the Centurion database snapshot into three new databases.
#
#   ./import.sh                                  # databases auth, world, characters
#   AUTH_DB=cauth WORLD_DB=cworld CHAR_DB=cchars ./import.sh
#   MYSQL="mysql -h 127.0.0.1 -u root -p" ./import.sh
#   BOTS=0 ./import.sh                           # without the bot accounts and characters
#
# The default names match the *DatabaseInfo defaults in worldserver.conf.dist and
# authserver.conf.dist. Needs MySQL 8.0 (the tables use utf8mb4_0900_ai_ci, which
# MariaDB does not have). mysql runs three times, so -p prompts three times; a
# ~/.my.cnf or MYSQL_PWD avoids that.
set -euo pipefail
cd "$(dirname "$0")"

MYSQL=${MYSQL:-mysql}
AUTH_DB=${AUTH_DB:-auth}
WORLD_DB=${WORLD_DB:-world}
CHAR_DB=${CHAR_DB:-characters}

for db in "$AUTH_DB" "$WORLD_DB" "$CHAR_DB"; do
    if [[ ! $db =~ ^[A-Za-z0-9_]+$ ]]; then
        echo "database name '$db' must be letters, digits and underscores" >&2
        exit 1
    fi
done

existing=$($MYSQL -N -B -e "SELECT table_schema, COUNT(*) FROM information_schema.tables
    WHERE table_schema IN ('$AUTH_DB', '$WORLD_DB', '$CHAR_DB') GROUP BY table_schema")
if [[ -n $existing && ${FORCE:-0} != 1 ]]; then
    echo "these databases already hold tables that this import would replace:" >&2
    echo "$existing" >&2
    echo "re-run with FORCE=1 to replace them" >&2
    exit 1
fi

$MYSQL -e "CREATE DATABASE IF NOT EXISTS \`$AUTH_DB\` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
           CREATE DATABASE IF NOT EXISTS \`$WORLD_DB\` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
           CREATE DATABASE IF NOT EXISTS \`$CHAR_DB\` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"

# The live databases were named legionnaireauth / centurionworld. Triggers, views and
# procedures refer to them by name, so those references follow your names.
rename() { sed -e "s/legionnaireauth/$AUTH_DB/g" -e "s/centurionworld/$WORLD_DB/g" "$1"; }

echo "loading $AUTH_DB, $CHAR_DB and $WORLD_DB ($(ls world/*.sql | wc -l) world files)"
{
    echo "USE \`$AUTH_DB\`;"
    rename auth/auth_schema.sql
    cat auth/auth_data.sql
    [[ ${BOTS:-1} == 0 ]] || cat auth/auth_bots.sql
    echo "USE \`$CHAR_DB\`;"
    rename characters/characters_schema.sql
    cat characters/characters_seed.sql
    [[ ${BOTS:-1} == 0 ]] || cat characters/characters_bots.sql
    echo "USE \`$WORLD_DB\`;"
    rename world/_routines.sql
    for f in world/*.sql; do
        [[ $f == world/_routines.sql ]] || cat "$f"
    done
} | $MYSQL

echo "done"
