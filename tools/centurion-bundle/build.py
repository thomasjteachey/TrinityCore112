#!/usr/bin/env python3
"""Regenerates centurion/sql/ from the live databases, the same way the 2026-09-26
snapshot was made (commits 1c7469f823 and d6e019e7e4):

  world/       centurionworld, one file per table in world_tables.txt (backup and
               scratch tables left out), tables over 45 MB split by key range into
               .1.sql/.2.sql, and _routines.sql (the procedures)
  characters/  centurioncharacters schema (no AUTO_INCREMENT counters), the seed (bot
               talent recipes + the nine Start<class> tournament templates, account 0)
               and the bots (accounts 76-79 with their gear, spells, pets and guilds)
  auth/        legionnaireauth schema, its reference data, the localhost realm row,
               and the four bot accounts with random password bytes

DEFINERs are stripped everywhere. Usage: build.py <out dir>  (reads the table lists
next to this script). Read-only on the live databases.
"""
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEF = os.path.expanduser('~/itemforge/my.cnf')
WORLD, CHARS, AUTH = 'centurionworld', 'centurioncharacters', 'legionnaireauth'
BOT_ACCOUNTS = (76, 77, 78, 79)
BOT_NAMES = {76: 'PLAYERBOTONE', 77: 'PLAYERBOTTWO', 78: 'PLAYERBOTTHREE', 79: 'PLAYERBOTFOUR'}
BOT_GUILDS = (2, 3)
TEMPLATES = tuple(range(13307, 13316))
SPLIT_BYTES = 45 * 1024 * 1024
DEFINER = re.compile(rb'DEFINER=`[^`]*`@`[^`]*` ?')
AUTOINC = re.compile(rb' AUTO_INCREMENT=\d+')
BASE = ['mysqldump', '--defaults-file=' + DEF, '--skip-comments', '--single-transaction',
        '--no-tablespaces', '--set-gtid-purged=OFF']


def q(sql):
    r = subprocess.run(['mysql', '--defaults-file=' + DEF, '-N', '-B', '-e', sql], capture_output=True, check=True)
    return r.stdout.decode()


def dump(*args):
    r = subprocess.run(BASE + list(args), capture_output=True)
    if r.returncode != 0:
        sys.exit('mysqldump %s failed: %s' % (' '.join(args), r.stderr.decode()[:500]))
    return DEFINER.sub(b'', r.stdout)


def lines(name):
    return [l.strip() for l in open(os.path.join(HERE, name)) if l.strip()]


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def ids(values):
    return ','.join(str(v) for v in values)


def world(out):
    for table in lines('world_tables.txt'):
        data = dump(WORLD, table)
        if len(data) <= SPLIT_BYTES:
            write(f'{out}/world/{table}.sql', data)
            continue
        # by the first primary-key column, into equal row counts
        pk = q(f"SELECT column_name FROM information_schema.key_column_usage WHERE table_schema='{WORLD}' "
               f"AND table_name='{table}' AND constraint_name='PRIMARY' ORDER BY ordinal_position LIMIT 1").strip()
        rows = int(q(f'SELECT COUNT(*) FROM `{WORLD}`.`{table}`'))
        parts = math.ceil(len(data) / SPLIT_BYTES)
        cuts = [q(f'SELECT `{pk}` FROM `{WORLD}`.`{table}` ORDER BY `{pk}` LIMIT 1 OFFSET {rows * i // parts}').strip()
                for i in range(1, parts)]
        bounds = [None] + cuts + [None]
        for i in range(parts):
            lo, hi = bounds[i], bounds[i + 1]
            where = ' AND '.join(c for c in ((f'`{pk}` >= {lo}' if lo else ''), (f'`{pk}` < {hi}' if hi else '')) if c)
            extra = [] if i == 0 else ['--no-create-info', '--skip-triggers']
            write(f'{out}/world/{table}.{i + 1}.sql', dump(*extra, '--where=' + where, WORLD, table))
        print(f'split {table}: {parts} parts at {pk} {cuts}')
    write(f'{out}/world/_routines.sql', dump('--no-data', '--no-create-info', '--routines', '--skip-triggers', WORLD))


def views(db):
    return q(f"SELECT table_name FROM information_schema.views WHERE table_schema='{db}' ORDER BY 1").split()


# The worldserver bans any secure addon missing from `addons` (WorldSession::ReadAddonsInfo),
# and every Blizzard_ addon is secure. Without these rows a fresh realm shows
# "Couldn't load Blizzard_CombatLog: Banned" and breaks talents, keybindings and more.
# Same rows as stock TrinityCore's sql/base/characters_database.sql; 1276933997 = 0x4C1C776D,
# the CRC of Blizzard's public key.
BLIZZARD_ADDONS = (
    b'\n-- Blizzard\'s own addons. The worldserver bans any secure addon missing from this table.\n'
    b'INSERT IGNORE INTO `addons` (`name`, `crc`) VALUES\n'
    + b',\n'.join(f"('Blizzard_{n}',1276933997)".encode() for n in (
        'AchievementUI', 'ArenaUI', 'AuctionUI', 'BarbershopUI', 'BattlefieldMinimap', 'BindingUI',
        'Calendar', 'CombatLog', 'CombatText', 'DebugTools', 'GlyphUI', 'GMChatUI', 'GMSurveyUI',
        'GuildBankUI', 'InspectUI', 'ItemSocketingUI', 'MacroUI', 'RaidUI', 'TalentUI',
        'TimeManager', 'TokenUI', 'TradeSkillUI', 'TrainerUI'))
    + b';\n')


def characters(out):
    tables = lines('chars_tables.txt')
    schema = dump('--no-data', '--routines', CHARS, *(tables + views(CHARS)))
    write(f'{out}/characters/characters_schema.sql', AUTOINC.sub(b'', schema))

    t = ids(TEMPLATES)
    seed = [b'-- Bot talent builds (playerbot_talent_recipe) and the tournament template characters\n'
            b'-- (Start<class>) that the createTournamentKit procedure copies spells, talents,\n'
            b'-- action bars and hunter pets from. The templates belong to no account.\n',
            dump('--no-create-info', CHARS, 'playerbot_talent_recipe'),
            dump('--no-create-info', f'--where=guid IN ({t})', CHARS, 'characters', 'character_spell', 'character_talent', 'character_action'),
            dump('--no-create-info', f'--where=owner IN ({t})', CHARS, 'character_pet'),
            dump('--no-create-info', f'--where=guid IN (SELECT id FROM character_pet WHERE owner IN ({t}))', CHARS, 'pet_spell'),
            f'UPDATE `characters` SET `account` = 0, `online` = 0 WHERE `guid` IN ({t});\n'.encode(),
            BLIZZARD_ADDONS]
    write(f'{out}/characters/characters_seed.sql', b''.join(seed))

    a = ids(BOT_ACCOUNTS)
    bots = f'SELECT guid FROM characters WHERE account IN ({a})'
    count = int(q(f'SELECT COUNT(*) FROM `{CHARS}`.characters WHERE account IN ({a})'))
    head = (f'-- Centurion\'s bots: {count} characters on accounts {a}, with their gear, spells, talents,\n'
            f'-- quests, reputation, hunter pets, guilds (AI Uprising, The Robot Masters) and zone-guardian\n'
            f'-- posts. Pair with auth/auth_bots.sql and centurion/conf/playerbots.conf.\n').encode()
    body = [head,
            dump('--no-create-info', f'--where=account IN ({a})', CHARS, 'characters'),
            dump('--no-create-info', f'--where=guid IN ({bots})', CHARS, 'character_achievement', 'character_action',
                 'character_glyphs', 'character_homebind', 'character_inventory', 'character_queststatus',
                 'character_queststatus_rewarded', 'character_reputation', 'character_skills', 'character_spell',
                 'character_talent', 'playerbot_zone_guardian'),
            dump('--no-create-info', f'--where=owner_guid IN ({bots})', CHARS, 'item_instance'),
            dump('--no-create-info', f'--where=owner IN ({bots})', CHARS, 'character_pet'),
            dump('--no-create-info', f'--where=guid IN (SELECT id FROM character_pet WHERE owner IN ({bots}))', CHARS, 'pet_spell'),
            dump('--no-create-info', f'--where=guildid IN ({ids(BOT_GUILDS)})', CHARS, 'guild', 'guild_rank', 'guild_member'),
            head,
            b'-- Snapshot of a running realm: nobody is online, and any inventory row whose item was saved\n'
            b'-- between two table reads is dropped rather than left pointing at nothing.\n',
            f'UPDATE `characters` SET `online` = 0 WHERE `account` IN ({a});\n'.encode(),
            (f'DELETE ci FROM `character_inventory` ci LEFT JOIN `item_instance` ii ON ii.`guid` = ci.`item`\n'
             f'  WHERE ii.`guid` IS NULL AND ci.`guid` IN (SELECT `guid` FROM `characters` WHERE `account` IN ({a}));\n').encode()]
    write(f'{out}/characters/characters_bots.sql', b''.join(body))
    return count


def auth(out, realm_block):
    tables = lines('auth_tables.txt')
    write(f'{out}/auth/auth_schema.sql', AUTOINC.sub(b'', dump('--no-data', '--routines', AUTH, *(tables + views(AUTH)))))
    data = dump('--no-create-info', AUTH, 'build_auth_key', 'build_executable_hash', 'build_info', 'rbac_default_permissions',
                'rbac_linked_permissions', 'rbac_permissions', 'updates', 'updates_include')
    write(f'{out}/auth/auth_data.sql', data + realm_block)
    counts = {}
    for row in q(f'SELECT account, COUNT(*) FROM `{CHARS}`.characters WHERE account IN ({ids(BOT_ACCOUNTS)}) GROUP BY account').splitlines():
        acct, n = row.split('\t')
        counts[int(acct)] = int(n)
    text = ('-- The bot accounts. Bots log in inside the worldserver, never through authserver, so their\n'
            '-- passwords are random bytes nobody knows.\n'
            'INSERT INTO `account` (`id`, `username`, `salt`, `verifier`, `email`, `reg_mail`, `last_ip`, `expansion`) VALUES\n'
            + ',\n'.join(f"({a}, '{BOT_NAMES[a]}', RANDOM_BYTES(32), RANDOM_BYTES(32), '', '', '127.0.0.1', 2)" for a in BOT_ACCOUNTS)
            + ';\nINSERT INTO `realmcharacters` (`realmid`, `acctid`, `numchars`) VALUES\n'
            # numchars is a tinyint: 255 at most, as the live realm stores it for 277
            + ',\n'.join(f'(1, {a}, {min(counts.get(a, 0), 255)})' for a in BOT_ACCOUNTS) + ';\n')
    write(f'{out}/auth/auth_bots.sql', text.encode())
    return counts


if __name__ == '__main__':
    out = sys.argv[1]
    realm_block = open(os.path.join(HERE, 'realm_block.sql'), 'rb').read()
    world(out)
    bots = characters(out)
    counts = auth(out, realm_block)
    print('world files', len(os.listdir(f'{out}/world')), 'bots', bots, counts)
