"""Generate the world-DB content for Blood Beneath the Arena (phases 1+2).

Writes bba_content.sql and bba_rollback.sql next to this file. Target DB is chosen at apply
time (the SQL has no USE). Everything lives in fresh id ranges that are empty on BOTH
centuriondevworld and centurionworld (checked 2026-10-01):

  creature_template   922000-922099      creature guids     922000-922299
  gameobject_template 900300-900319      gameobject guids   5331000-5331299
  quest               60100              item               204100
  gossip menus / npc_text 60100-60119    map 1685 / zone 30609

Nothing outside those ranges is modified, so the rollback is a plain DELETE of the ranges.
Stock templates the customer used for his mock-up are NEVER edited - each role gets its own
copy (owner's rule: editing a stock template changes it everywhere in the world).
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
MAP, ZONE = 1685, 30609
START = (-13238.5, 203.0, -66.8, 0.0)              # inside, between the four cages
ENTRANCE = (-13277.4, 127.372, 26.1418, 4.25)      # map 0, Gurubashi Arena gate (stock GurubashiArena tele)
EXIT_NPC = (-13242.0, 198.5, -67.0, 0.6)
QUEST, ITEM = 60100, 204100
GOLD = 10000


def q(s):
    if s is None:
        return 'NULL'
    return "'" + str(s).replace('\\', '\\\\').replace("'", "\\'") + "'"


# entry, source, name, subname, role, class, level, rank, hp, hit, gold(min,max), extra
# role: friendly / boss / support / trash / add
NPCS = [
    (922000, 11407, "Var'jun", None, 'friendly', None, 60, 1, 25000, 0, (0, 0)),
    (922001, 28931, 'Blightblood', None, 'friendly', None, 60, 1, 50000, 0, (0, 0)),
    (922002, 15863, 'Tunnel Watcher', 'Blood Beneath the Arena', 'friendly', None, 60, 1, 20000, 0, (0, 0)),
    (922003, 15863, 'Tunnel Watcher', 'The Way Up', 'friendly', None, 60, 1, 20000, 0, (0, 0)),
    (922004, 10116, 'Bloodbound Captive', None, 'friendly', None, 60, 0, 3000, 0, (0, 0)),
    (922005, 10117, 'Tortured Captive', None, 'friendly', None, 60, 0, 3000, 0, (0, 0)),
    # Sand trial
    (922010, 7267, "Zul'kash the Buried", None, 'boss', 1, 61, 1, 70000, 450, (3, 5)),
    (922011, 7271, "Raka'jin the Dustcaller", None, 'support', 2, 61, 1, 25000, 250, (1, 2)),
    (922012, 5649, 'Bloodbound Sand Berserker', None, 'trash', 1, 60, 1, 9000, 350, (0.4, 0.8)),
    (922013, 5648, 'Bloodbound Sand Hexer', None, 'trash', 8, 60, 1, 7000, 250, (0.4, 0.8)),
    (922014, 5650, 'Bloodbound Sand Medic', None, 'trash', 2, 60, 1, 7500, 250, (0.4, 0.8)),
    # Ice trial
    (922020, 28494, "Rok'thul the Iceblood", None, 'boss', 1, 61, 1, 72000, 500, (3, 5)),
    (922021, 28417, 'Yekra Frostcaller', None, 'support', 2, 61, 1, 25000, 250, (1, 2)),
    (922022, 28388, 'Bloodbound Ice Berserker', None, 'trash', 1, 60, 1, 9000, 350, (0.4, 0.8)),
    (922023, 29237, 'Bloodbound Ice Hexer', None, 'trash', 8, 60, 1, 7000, 250, (0.4, 0.8)),
    (922024, 28504, 'Bloodbound Ice Medic', None, 'trash', 2, 60, 1, 7500, 250, (0.4, 0.8)),
    # Forest trial
    (922030, 24549, "Mok'ra the Thorned", None, 'boss', 1, 61, 1, 75000, 525, (3, 5)),
    (922031, 23581, "Kez'ani Rootspeaker", None, 'support', 2, 61, 1, 25000, 250, (1, 2)),
    (922032, 23597, 'Bloodbound Forest Berserker', None, 'trash', 1, 60, 1, 9000, 350, (0.4, 0.8)),
    (922033, 23596, 'Bloodbound Forest Hexer', None, 'trash', 8, 60, 1, 7000, 250, (0.4, 0.8)),
    (922034, 24179, 'Bloodbound Forest Medic', None, 'trash', 2, 60, 1, 7500, 250, (0.4, 0.8)),
    # Patrols - one copy per Shadowpine template the customer placed
    (922040, 16340, 'Bloodbound Cutthroat', None, 'trash', 1, 60, 1, 8000, 300, (0.4, 0.8)),
    (922041, 16346, 'Bloodbound Hexxer', None, 'trash', 8, 60, 1, 6500, 250, (0.4, 0.8)),
    (922042, 16343, 'Bloodbound Oracle', None, 'trash', 2, 60, 1, 7000, 250, (0.4, 0.8)),
    (922043, 16344, 'Bloodbound Headhunter', None, 'trash', 1, 60, 1, 8000, 300, (0.4, 0.8)),
    (922044, 16341, 'Bloodbound Witch', None, 'trash', 8, 60, 1, 6500, 250, (0.4, 0.8)),
    (922045, 16345, 'Bloodbound Catlord', None, 'trash', 1, 60, 1, 8500, 320, (0.4, 0.8)),
    (922046, 16469, 'Bloodbound Shadowcaster', None, 'trash', 8, 60, 1, 6500, 250, (0.4, 0.8)),
    # Zalvaxa
    (922050, 16358, 'Zalvaxa the Wicked', None, 'boss', 8, 61, 1, 95000, 550, (5, 8)),
    (922051, 16360, 'Risen Bloodbound', None, 'add', 1, 60, 0, 1500, 150, (0, 0)),
    # Twins
    (922060, 14515, 'Thraxia', 'Bloodblade Assassin', 'boss', 4, 61, 1, 90000, 600, (3, 5)),
    (922061, 14510, 'Malizzia', 'Mistress of the Black Flame', 'boss', 8, 61, 1, 88000, 500, (3, 5)),
    # Finale
    (922070, 14517, "Vraka'ti the Blood Gorger", 'Mother of the Bloodbound', 'boss', 8, 61, 1, 110000, 650, (0, 0)),
    (922071, 16357, "Anok'Suten", 'Mistress of the Blood Web', 'boss', 1, 61, 1, 65000, 700, (10, 15)),
]

# charm, disorient, fear, sleep, polymorph, banish, shackle, turn, horror, sapped
BOSS_IMMUNE = 550175251

GOSSIP = {
    # menu: (npc_text, text, [(option, text, action_menu)])
    60100: (60100, "You came for treasure? Then you are already in danger. Open the cage first; questions can wait.",
            [(0, "Who are you?", 60101)]),
    60101: (60101, "I taught young priests to heal and to cleanse. The Bloodbound took me because I know the old Gurubashi rites well enough to see what this ritual truly is.$B$BFour pillars. One cauldron. Too much blood. We cleanse the source first, then we cut out the hands that keep feeding it.", []),
    60102: (60102, "You smell like open air. Strange... I had almost forgotten what it smelled like.",
            [(0, "Why were you in chains?", 60103)]),
    60103: (60103, "They wanted my blood to call Kha'Muda. They say the Chosen will rise. I say the only thing that should rise is us.", []),
    60104: (60104, "Drums beneath the sand, and screams beneath the drums. The Bloodbound have dug something terrible under this arena.$B$BGo down with four friends at your back, or do not go at all.",
            [(0, "Take me beneath the arena.", 0)]),
    60105: (60105, "The way back up is behind me. No one will think less of you.",
            [(0, "Take me back up to the arena.", 0)]),
}
NPC_MENU = {922000: 60100, 922001: 60102, 922002: 60104, 922003: 60105}

Y, S = 14, 12  # monster yell / say
TEXTS = {
    922000: [(S, "You came for treasure? Then you are already in danger. Open the cage first; questions can wait.", 'freed'),
             (S, "The trolls call this place a temple. It is a slaughterhouse. I will cleanse the pillar. You will keep the killers away from me.", 'quest'),
             (S, "Good. They expected frightened prisoners, not five armed problems.", 'after patrols'),
             (S, "Easy, friend. We are not here to finish what they started.", 'meeting Blightblood'),
             (S, "Someone has to.", 'reply to Blightblood'),
             (S, "Four pillars. One cauldron. Too much blood. We cleanse the source first, then we cut out the hands that keep feeding it.", 'before ritual'),
             (S, "Bang the gong when your minds are clear and your mana is full. Once it rings, the cult will know we are here.", 'gong'),
             (S, "The desert tribe is broken. Take the troll's boon, but do not grow careless.", 'after Sand'),
             (S, "Cold steel and colder magic. One tribe remains.", 'after Ice'),
             (S, "The pillar is clean. Something underneath it is not. I can feel it.", 'after Forest'),
             (Y, "Zalvaxa! Your dead have been fed to their own grave. Come out and face the living!", 'Zalvaxa taunt'),
             (S, "No more drums. No more bones. Upstairs, quickly. The arena is becoming a coffin.", 'after Zalvaxa'),
             (S, "You did not save the arena. You saved the people beneath it. Remember the difference.", 'at exit')],
    922001: [(S, "You smell like open air. Strange... I had almost forgotten what it smelled like.", 'first meeting'),
             (S, "Var'jun. Little teacher. Still saving people who cannot save themselves?", "greets Var'jun"),
             (S, "The chains are broken, little priest. But the blood still remembers my name.", "reply to Var'jun"),
             (S, "They wanted my blood to call Kha'Muda. They say the Chosen will rise. I say the only thing that should rise is us.", 'explain sacrifice'),
             (S, "The sand taught you to move. Take its wind, then.", 'boon 1'),
             (S, "The first champion falls. My blood remembers courage.", 'boon 2'),
             (S, "Cold teaches patience. Do not spend all your strength in one breath.", 'boon 3'),
             (S, "Roots break stone slowly. You have broken them quickly.", 'boon 4'),
             (S, "The jungle tried to keep you. Now let it lend you its teeth.", 'boon 5'),
             (S, "The dead are quiet. Good. Climb. The thing on the throne is not a troll.", 'after Zalvaxa'),
             (S, "You are close. Whatever waits above has been waiting for you.", 'after twins'),
             (S, "Go. Do not look back at this place. Blood remembers, but it does not command you.", 'final exit'),
             (S, "The sands teach your feet where the eyes cannot.", 'Boon of the Dune'),
             (S, "Strike before the enemy knows you moved.", 'Boon of the Buried Fang'),
             (S, "Cold makes the mind patient.", 'Boon of the Frozen Breath'),
             (S, "Preserve strength until the moment it matters.", 'Boon of the Frostbound'),
             (S, "The jungle lends you its heart.", 'Boon of the Living Root'),
             (S, "Even the dead can be pruned.", 'Boon of the Thorned Soul'),
             (S, "You have beaten the grave.", 'Boon of Defiance'),
             (S, "No chain holds you now.", 'Boon of the Unbound')],
}
for boss, support, tribe in ((922010, 922011, ('Sand', "Zul'kash the Buried")),
                             (922020, 922021, ('Ice', "Rok'thul the Iceblood")),
                             (922030, 922031, ('Forest', "Mok'ra the Thorned"))):
    t, n = tribe
    TEXTS[boss] = [(Y, "%s blood does not bend. Come, strangers." % t, 'pull'),
                   (Y, "Enough! I will tear the blood from your bones myself!", '30%'),
                   (Y, "The %s remembers..." % t.lower(), 'death')]
    TEXTS[support] = [(Y, "Hear the drums. %s does not fight alone." % n, 'support buff'),
                      (Y, "No death yet. Not while I still have blood to spend.", 'switches to heal'),
                      (Y, "My spirit... is empty. Give me one more breath...", '0 mana')]
TEXTS[922050] = [(Y, "Rise, little failures. Rise until the living have nowhere left to stand.", 'summoning channel'),
                 (Y, "More. I need more bones.", 'zombie wave'),
                 (Y, "Let the dead point the way.", 'Bone Spear'),
                 (Y, "Your flesh has already begun to leave you.", 'Grave Rot'),
                 (Y, "Smile, hero. Your corpse will need a face.", 'Voodoo Hex'),
                 (Y, "NO! You do not touch my ritual!", 'interrupted'),
                 (Y, "Fine. Come collect your dead with your own hands.", 'teleport middle'),
                 (Y, "You break my servants, but you cannot break the grave.", 'low HP'),
                 (Y, "The blood... was never enough...", 'death'),
                 (Y, "Living? You brought five warm bodies into my temple. I can fix that.", "reply to Var'jun's taunt")]
TEXTS[922060] = [(Y, "First blood is mine.", 'Garrote'), (Y, "Eyes up, fool.", 'Gouge'),
                 (Y, "You cannot fight what you cannot see.", 'Blind'), (Y, "Stand close. Die together.", 'Blade Flurry'),
                 (Y, "Malizzia! Wake me when they are dead.", 'first twin death'), (Y, "A clean... finish...", 'death')]
TEXTS[922061] = [(Y, "Burn from the inside.", 'Shadow Bolt'), (Y, "Pain is the only honest teacher.", 'Curse of Agony'),
                 (Y, "Run. It only makes the ritual easier.", 'Fear'), (Y, "Your life belongs to me.", 'Drain Life'),
                 (Y, "Sister, breathe. I have not finished the lesson.", 'resurrection timer'),
                 (Y, "Impossible... the web promised us victory...", 'death')]
TEXTS[922070] = [(Y, "So the arena sends champions below the floor. How generous.", 'first meeting'),
                 (Y, "Your blood will fill the cauldron.", 'combat start'),
                 (Y, "Choose who among you will feed the Chosen.", 'Sacrificial Mark'),
                 (Y, "Give me your pain. Give me your fear.", 'Blood Drain'),
                 (Y, "Children of the Bloodbound, tear them apart!", 'adds'),
                 (Y, "Stop! Enough. You have served your purpose.", '25% reveal'),
                 (Y, "You were never meant to kill me. You were meant to open the door.", 'disguise cracking'),
                 (Y, "Look carefully, little mortals. Your Blood Mother is a lie.", 'reveal')]
TEXTS[922071] = [(Y, "The troll was a costume. The blood was the key.", 'Unmasking'),
                 (Y, "Be still. The web knows where you belong.", 'Web Cocoon'),
                 (Y, "Every road ends in a web.", 'Shadow Web'),
                 (Y, "Come, children. Feed.", 'Nerubian Swarm'),
                 (Y, "You are beneath me.", 'Piercing Legs'),
                 (Y, "Your hearts beat on my command.", 'Blood Web'),
                 (Y, "Enough. I have tolerated you long enough.", 'enrage'),
                 (Y, "No... the web... survives...", 'death')]

# customer's mock-up on map 0 (centuriondevworld guids) -> instance copies.
# New guid = old + 760 (921245..921439 -> 922005..922199). Left out on purpose:
#   921245 / 921352 / 921439  extra Var'jun spawns = his escort route markers (pillar z-93,
#                             Blightblood z-79.5, top z-36) - kept as notes for phase 3
#   2449 Addled Leper, 921380/921381/921436/921437 Amani wards, 921382/921383 Ice Spheres -
#                             stock NPCs with their own AI/scripts (wards heal hostiles)
MOCK_REMAP = {11407: 922000, 28931: 922001, 10116: 922004, 10117: 922005,
              7267: 922010, 7271: 922011, 5648: 922013, 5650: 922014,
              28494: 922020, 28417: 922021, 28388: 922022, 29237: 922023, 28504: 922024,
              24549: 922030, 23581: 922031, 23597: 922032, 23596: 922033, 24179: 922034,
              16340: 922040, 16346: 922041, 16343: 922042, 16344: 922043, 16341: 922044,
              16345: 922045, 16469: 922046, 16358: 922050, 16360: 922051,
              31649: 922060, 23863: 922061, 16357: 922070}
MOCK_GUIDS = [921248, 921314, 921315, 921317, 921318, 921319, 921322, 921323, 921324, 921325,
              921326, 921327, 921328, 921329, 921330, 921332, 921337, 921338, 921340, 921341,
              921343, 921344, 921345, 921346, 921347, 921348, 921349, 921350, 921351, 921353,
              921356, 921388, 921401, 921425, 921426, 921427, 921431, 921438]
GO_LO, GO_HI, GO_OFF = 5330534, 5330721, 466        # -> 5331000..5331187


def build():
    o = []
    w = o.append
    w("-- Blood Beneath the Arena - phases 1+2 content (generated by gen_bba_sql.py)")
    w("-- Map %d / zone %d. Fresh id ranges only; see bba_rollback.sql." % (MAP, ZONE))
    w("-- Var'jun escort-route markers from the mock-up (map 0 coords == map %d coords):" % MAP)
    w("--   cage (-13232.5,201.6,-67.0)  Blightblood (-13252.8,262.7,-79.5)")
    w("--   pillar (-13224.5,268.6,-93.0)  top (-13229.5,229.8,-36.1)")
    w("SET @OLD_SQL_MODE=@@SQL_MODE; SET SQL_MODE='';")
    w("START TRANSACTION;")
    w(open(os.path.join(HERE, 'bba_rollback_body.sql')).read())

    # ---- creature templates ------------------------------------------------
    w("DROP TEMPORARY TABLE IF EXISTS bba_ct; CREATE TEMPORARY TABLE bba_ct LIKE creature_template;")
    w("DROP TEMPORARY TABLE IF EXISTS bba_cta; CREATE TEMPORARY TABLE bba_cta LIKE creature_template_addon;")
    w("DROP TEMPORARY TABLE IF EXISTS bba_cte; CREATE TEMPORARY TABLE bba_cte LIKE creature_equip_template;")
    w("DROP TEMPORARY TABLE IF EXISTS bba_ctm; CREATE TEMPORARY TABLE bba_ctm LIKE creature_template_movement;")
    for (e, src, name, sub, role, cls, lvl, rank, hp, hit, gold) in NPCS:
        w("INSERT INTO bba_ct SELECT * FROM creature_template WHERE entry=%d;" % src)
        sets = ["entry=%d" % e, "name=%s" % q(name), "subname=%s" % q(sub), "IconName=NULL",
                "difficulty_entry_1=0", "difficulty_entry_2=0", "difficulty_entry_3=0",
                "KillCredit1=0", "KillCredit2=0", "minlevel=%d" % lvl, "maxlevel=%d" % lvl, "exp=0",
                "`rank`=%d" % rank, "lootid=0", "pickpocketloot=0", "skinloot=0",
                "mingold=%d" % int(gold[0] * GOLD), "maxgold=%d" % int(gold[1] * GOLD),
                "AIName=''", "ScriptName=''", "MovementType=0", "dynamicflags=0", "VehicleId=0",
                "PetSpellDataId=0", "RacialLeader=0", "ExperienceModifier=1", "RegenHealth=1",
                "VerifiedBuild=0"]
        if role == 'friendly':
            menu = NPC_MENU.get(e, 0)
            flag = {922000: 3}.get(e, 1 if menu else 0)
            sets += ["faction=35", "npcflag=%d" % flag, "gossip_menu_id=%d" % menu, "unit_flags=0",
                     "type_flags=%d" % (2 if e in (922002, 922003) else 0),
                     "mechanic_immune_mask=0", "flags_extra=0"]
            if e in (922002, 922003):
                sets.append("AIName='SmartAI'")
        else:
            sets += ["faction=14", "npcflag=0", "gossip_menu_id=0", "unit_class=%d" % cls,
                     "BaseAttackTime=2000", "RangeAttackTime=2000", "unit_flags=0",
                     "unit_flags2=%d" % (0 if role == 'support' else 2048), "type_flags=0",
                     "ManaModifier=%s" % ('2' if role == 'support' else '1'), "ArmorModifier=1",
                     "mechanic_immune_mask=%d" % (BOSS_IMMUNE if role == 'boss' else 0), "flags_extra=0"]
            if e in (922051, 922071):
                sets.append("type=6")
            else:
                sets.append("type=7")
        w("UPDATE bba_ct SET %s WHERE entry=%d;" % (', '.join(sets), src))
        w("INSERT INTO bba_cta SELECT * FROM creature_template_addon WHERE entry=%d; UPDATE bba_cta SET entry=%d, path_id=0 WHERE entry=%d;" % (src, e, src))
        w("INSERT INTO bba_cte SELECT * FROM creature_equip_template WHERE CreatureID=%d; UPDATE bba_cte SET CreatureID=%d WHERE CreatureID=%d;" % (src, e, src))
        w("INSERT INTO bba_ctm SELECT * FROM creature_template_movement WHERE CreatureId=%d; UPDATE bba_ctm SET CreatureId=%d WHERE CreatureId=%d;" % (src, e, src))
    w("INSERT INTO creature_template SELECT * FROM bba_ct;")
    w("INSERT INTO creature_template_addon SELECT * FROM bba_cta;")
    w("INSERT INTO creature_equip_template SELECT * FROM bba_cte;")
    w("INSERT INTO creature_template_movement SELECT * FROM bba_ctm;")
    # HP / melee from creature_classlevelstats (exp 0): HP = target, avg hit ~ target (unmitigated)
    for (e, src, name, sub, role, cls, lvl, rank, hp, hit, gold) in NPCS:
        dmg = ("ct.DamageModifier=ROUND(GREATEST(0.1,(%d/(ct.BaseAttackTime/1000)-s.attackpower/14)/(s.damage_base*1.25)),3)" % hit) if hit else "ct.DamageModifier=1"
        w("UPDATE creature_template ct JOIN creature_classlevelstats s ON s.level=ct.minlevel AND s.class=ct.unit_class "
          "SET ct.HealthModifier=ROUND(%d/s.basehp0,3), %s WHERE ct.entry=%d;" % (hp, dmg, e))

    # ---- texts -----------------------------------------------------------
    for e, rows in TEXTS.items():
        for gid, (typ, txt, comment) in enumerate(rows):
            w("INSERT INTO creature_text (CreatureID,GroupID,ID,Text,Type,Language,Probability,Emote,Duration,Sound,BroadcastTextId,TextRange,comment) "
              "VALUES (%d,%d,0,%s,%d,0,100,0,0,0,0,0,%s);" % (e, gid, q(txt), typ, q('BBA %d - %s' % (e, comment))))

    # ---- gossip ----------------------------------------------------------
    for menu, (tid, text, opts) in GOSSIP.items():
        w("INSERT INTO npc_text (ID,text0_0,text0_1,BroadcastTextID0,lang0,Probability0,VerifiedBuild) VALUES (%d,%s,%s,0,0,1,0);" % (tid, q(text), q(text)))
        w("INSERT INTO gossip_menu (MenuID,TextID,VerifiedBuild) VALUES (%d,%d,0);" % (menu, tid))
        for oid, otext, action in opts:
            w("INSERT INTO gossip_menu_option (MenuID,OptionID,OptionIcon,OptionText,OptionBroadcastTextID,OptionType,OptionNpcFlag,ActionMenuID,ActionPoiID,BoxCoded,BoxMoney,BoxText,BoxBroadcastTextID,VerifiedBuild) "
              "VALUES (%d,%d,0,%s,0,1,1,%d,0,0,0,'',0,0);" % (menu, oid, q(otext), action))

    # ---- entrance / exit teleports (SmartAI gossip select) -------------
    for e, menu, mapid, pos, what in ((922002, 60104, MAP, START, 'enter'), (922003, 60105, 0, ENTRANCE, 'leave')):
        w("INSERT INTO smart_scripts (entryorguid,source_type,id,link,event_type,event_phase_mask,event_chance,event_flags,event_param1,event_param2,event_param3,event_param4,event_param5,"
          "action_type,action_param1,action_param2,action_param3,action_param4,action_param5,action_param6,target_type,target_param1,target_param2,target_param3,target_param4,target_x,target_y,target_z,target_o,comment) VALUES "
          "(%d,0,0,0,62,0,100,0,%d,0,0,0,0,72,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,%s),"
          "(%d,0,1,0,62,0,100,0,%d,0,0,0,0,62,%d,0,0,0,0,0,7,0,0,0,0,%s,%s,%s,%s,%s);"
          % (e, menu, q('Tunnel Watcher - gossip - close'), e, menu, mapid, pos[0], pos[1], pos[2], pos[3],
             q('Tunnel Watcher - gossip - teleport (%s Blood Beneath the Arena)' % what)))

    # ---- instance --------------------------------------------------------
    w("INSERT INTO instance_template (map,parent,script,allowMount) VALUES (%d,0,'',0);" % MAP)
    w("INSERT INTO access_requirement (mapId,difficulty,level_min,level_max,item_level,item,item2,quest_done_A,quest_done_H,completed_achievement,quest_failed_text,comment) "
      "VALUES (%d,0,58,0,0,0,0,0,0,0,NULL,'Blood Beneath the Arena');" % MAP)
    w("INSERT INTO graveyard_zone (ID,GhostZone,Faction,Comment) VALUES (1458,%d,0,'Blood Beneath the Arena - Gurubashi Arena GY');" % ZONE)

    # ---- spawns: customer's mock-up copied into the instance ------------
    w("DROP TEMPORARY TABLE IF EXISTS bba_cr; CREATE TEMPORARY TABLE bba_cr LIKE creature;")
    w("INSERT INTO bba_cr SELECT * FROM creature WHERE map=0 AND guid IN (%s);" % ','.join(map(str, MOCK_GUIDS)))
    case = ' '.join('WHEN %d THEN %d' % kv for kv in MOCK_REMAP.items())
    w("UPDATE bba_cr SET guid=guid+760, id=CASE id %s ELSE 0 END, map=%d, zoneId=0, areaId=0, spawnMask=1, phaseMask=1, modelid=0, spawntimesecs=86400, wander_distance=0, MovementType=0;" % (case, MAP))
    w("UPDATE bba_cr c LEFT JOIN creature_equip_template e ON e.CreatureID=c.id AND e.ID=1 SET c.equipment_id=IF(e.CreatureID IS NULL,0,1);")
    w("INSERT INTO creature SELECT * FROM bba_cr WHERE id<>0;")
    # Tunnel Watchers (one outside on map 0, one inside by the start)
    w("DROP TEMPORARY TABLE IF EXISTS bba_cr2; CREATE TEMPORARY TABLE bba_cr2 LIKE creature;")
    for g, e, mapid, pos in ((922200, 922002, 0, ENTRANCE), (922201, 922003, MAP, EXIT_NPC)):
        w("DELETE FROM bba_cr2; INSERT INTO bba_cr2 SELECT * FROM creature WHERE guid=921431;")
        w("UPDATE bba_cr2 SET guid=%d, id=%d, map=%d, zoneId=0, areaId=0, spawnMask=1, phaseMask=1, modelid=0, equipment_id=1, "
          "position_x=%s, position_y=%s, position_z=%s, orientation=%s, spawntimesecs=300, wander_distance=0, MovementType=0;"
          % (g, e, mapid, pos[0], pos[1], pos[2], pos[3]))
        w("INSERT INTO creature SELECT * FROM bba_cr2;")

    # ---- gameobjects ---------------------------------------------------
    w("DROP TEMPORARY TABLE IF EXISTS bba_gt; CREATE TEMPORARY TABLE bba_gt LIKE gameobject_template;")
    w("INSERT INTO bba_gt SELECT * FROM gameobject_template WHERE entry=180526;")
    w("UPDATE bba_gt SET entry=900300, name='Bloodbound Gong', Data2=0, AIName='', ScriptName='', VerifiedBuild=0 WHERE entry=180526;")
    w("INSERT INTO bba_gt SELECT * FROM gameobject_template WHERE entry=185913;")
    w("UPDATE bba_gt SET entry=900301, type=5, name='Skull Pile', Data0=0, Data1=0, Data2=0, Data3=0, Data4=0, Data5=0, Data6=0, Data7=0, Data8=0, Data9=0, Data10=0, AIName='', ScriptName='', VerifiedBuild=0 WHERE entry=185913;")
    w("INSERT INTO gameobject_template SELECT * FROM bba_gt;")
    w("DROP TEMPORARY TABLE IF EXISTS bba_go; CREATE TEMPORARY TABLE bba_go LIKE gameobject;")
    w("INSERT INTO bba_go SELECT * FROM gameobject WHERE map=0 AND guid BETWEEN %d AND %d;" % (GO_LO, GO_HI))
    w("UPDATE bba_go SET guid=guid+%d, map=%d, zoneId=0, areaId=0, spawnMask=1, phaseMask=1, spawntimesecs=86400, id=CASE id WHEN 180526 THEN 900300 WHEN 185913 THEN 900301 ELSE id END;" % (GO_OFF, MAP))
    w("INSERT INTO gameobject SELECT * FROM bba_go;")
    w("DROP TEMPORARY TABLE IF EXISTS bba_ga; CREATE TEMPORARY TABLE bba_ga LIKE gameobject_addon;")
    w("INSERT INTO bba_ga SELECT * FROM gameobject_addon WHERE guid BETWEEN %d AND %d; UPDATE bba_ga SET guid=guid+%d;" % (GO_LO, GO_HI, GO_OFF))
    w("INSERT INTO gameobject_addon SELECT * FROM bba_ga;")

    # ---- quest + reward --------------------------------------------------
    desc = ("The trolls call this place a temple. It is a slaughterhouse.$B$B"
            "The Bloodbound have chained innocents beneath the arena and mean to drain their blood into the ritual cauldron. "
            "Blightblood, the great one they keep in chains, is to be the next sacrifice - they believe his blood will call Kha'Muda, the Chosen of the Blood Loa.$B$B"
            "I will cleanse the pillar. You will keep the killers away from me. Then we cut out the hands that keep feeding it: the tribal champions, their necromancer, and whoever sits on that throne.")
    w("INSERT INTO quest_template (ID,QuestType,QuestLevel,MinLevel,QuestSortID,QuestInfoID,SuggestedGroupNum,RewardXPDifficulty,Flags,"
      "RewardItem1,RewardAmount1,LogTitle,LogDescription,QuestDescription,QuestCompletionLog,RequiredNpcOrGo1,RequiredNpcOrGoCount1,ObjectiveText1,VerifiedBuild) VALUES "
      "(%d,2,60,58,%d,81,5,5,8,%d,1,%s,%s,%s,%s,922071,1,%s,0);"
      % (QUEST, ZONE, ITEM, q('Blood Beneath the Arena'),
         q("Rescue Var'jun, cleanse the blood ritual, break the Bloodbound's champions, and stop the sacrifice."),
         q(desc), q("Return to Var'jun."), q('Leader of the Bloodbound defeated')))
    w("INSERT INTO quest_request_items (ID,EmoteOnComplete,EmoteOnIncomplete,CompletionText,VerifiedBuild) VALUES (%d,1,1,%s,0);"
      % (QUEST, q("Is it done? Did the one on the throne fall?")))
    w("INSERT INTO quest_offer_reward (ID,Emote1,RewardText,VerifiedBuild) VALUES (%d,1,%s,0);"
      % (QUEST, q("You did not save the arena. You saved the people beneath it. Remember the difference.$B$BTake this. The Bloodbound will not be needing it.")))
    w("INSERT INTO creature_queststarter (id,quest) VALUES (922000,%d);" % QUEST)
    w("INSERT INTO creature_questender (id,quest) VALUES (922000,%d);" % QUEST)
    w("DROP TEMPORARY TABLE IF EXISTS bba_it; CREATE TEMPORARY TABLE bba_it LIKE item_template;")
    w("INSERT INTO bba_it SELECT * FROM item_template WHERE entry=20602;")
    w("UPDATE bba_it SET entry=%d, name='Bloodbound Treasure Box', Quality=3, ItemLevel=60, RequiredLevel=0, bonding=1, "
      "BuyPrice=0, SellPrice=0, maxcount=0, stackable=1, minMoneyLoot=%d, maxMoneyLoot=%d, "
      "description=%s, VerifiedBuild=0 WHERE entry=20602;" % (ITEM, 15 * GOLD, 25 * GOLD, q("Var'jun's thanks for stopping the sacrifice beneath the arena.")))
    w("INSERT INTO item_template SELECT * FROM bba_it;")

    w("COMMIT;")
    w("SET SQL_MODE=@OLD_SQL_MODE;")
    return '\n'.join(o) + '\n'


ROLLBACK = """-- remove everything Blood Beneath the Arena added (fresh id ranges only)
DELETE FROM creature WHERE guid BETWEEN 922000 AND 922299;
DELETE FROM creature_addon WHERE guid BETWEEN 922000 AND 922299;
DELETE FROM gameobject WHERE guid BETWEEN 5331000 AND 5331299;
DELETE FROM gameobject_addon WHERE guid BETWEEN 5331000 AND 5331299;
DELETE FROM gameobject_template WHERE entry BETWEEN 900300 AND 900319;
DELETE FROM creature_text WHERE CreatureID BETWEEN 922000 AND 922099;
DELETE FROM smart_scripts WHERE source_type=0 AND entryorguid BETWEEN 922000 AND 922099;
DELETE FROM creature_template_addon WHERE entry BETWEEN 922000 AND 922099;
DELETE FROM creature_equip_template WHERE CreatureID BETWEEN 922000 AND 922099;
DELETE FROM creature_template_movement WHERE CreatureId BETWEEN 922000 AND 922099;
DELETE FROM creature_queststarter WHERE quest=60100;
DELETE FROM creature_questender WHERE quest=60100;
DELETE FROM creature_template WHERE entry BETWEEN 922000 AND 922099;
DELETE FROM gossip_menu_option WHERE MenuID BETWEEN 60100 AND 60119;
DELETE FROM gossip_menu WHERE MenuID BETWEEN 60100 AND 60119;
DELETE FROM npc_text WHERE ID BETWEEN 60100 AND 60119;
DELETE FROM quest_offer_reward WHERE ID=60100;
DELETE FROM quest_request_items WHERE ID=60100;
DELETE FROM quest_template_addon WHERE ID=60100;
DELETE FROM quest_template WHERE ID=60100;
DELETE FROM item_template WHERE entry=204100;
DELETE FROM instance_template WHERE map=1685;
DELETE FROM access_requirement WHERE mapId=1685;
DELETE FROM graveyard_zone WHERE GhostZone=30609;
"""

if __name__ == '__main__':
    open(os.path.join(HERE, 'bba_rollback_body.sql'), 'w', newline='\n').write(ROLLBACK)
    open(os.path.join(HERE, 'bba_rollback.sql'), 'w', newline='\n').write(ROLLBACK)
    sql = build()
    open(os.path.join(HERE, 'bba_content.sql'), 'w', newline='\n', encoding='utf-8').write(sql)
    print('bba_content.sql: %d lines, %d bytes' % (sql.count('\n'), len(sql)))
