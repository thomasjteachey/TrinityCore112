/*
 * Server-side registration for the four player-collision auras.
 *
 * Human players are handled entirely by the client tweak - movement is
 * client-authoritative, so a client clipping its own movement IS the mechanic.
 * Playerbots have no client and are moved server-side by the motion master, so
 * without this they walk straight through blockers.
 *
 * These scripts only register and unregister the wearer, tagging which of the
 * four rules the aura represents. The geometry and the rule matrix live in
 * PlayerCollisionServer, and the bot movement code consults it when it builds a
 * movement segment. The spell ids live only in spell_script_names - the core
 * never hardcodes them.
 *
 *   90210 Obstruction   -> RULE_BLOCK_ENEMIES    (one-way, enemies)
 *   90211 Immovable     -> RULE_BLOCK_ALL        (one-way, everyone)
 *   90212 Bodycheck     -> RULE_COLLIDE_ENEMIES  (mutual, enemies)
 *   90213 Solid Form    -> RULE_COLLIDE_ALL      (mutual, everyone)
 *
 * Written as four explicit classes rather than a template: PrepareAuraScript and
 * RegisterSpellScript both key off the concrete class name, and this has to
 * compile first time (there is no local compiler here).
 */

#include "ScriptMgr.h"
#include "SpellScript.h"
#include "SpellAuraEffects.h"
#include "PlayerCollisionServer.h"
#include "Unit.h"

#define COLLISION_AURA_SCRIPT(CLASS, RULE)                                        \
class CLASS : public AuraScript                                                   \
{                                                                                 \
    PrepareAuraScript(CLASS);                                                     \
                                                                                  \
    void OnApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)     \
    {                                                                             \
        if (Unit* target = GetTarget())                                           \
            PlayerCollisionServer::Add(target, GetId(), RULE);                     \
    }                                                                             \
                                                                                  \
    void OnRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)    \
    {                                                                             \
        if (Unit* target = GetTarget())                                           \
            PlayerCollisionServer::Remove(target, GetId());                        \
    }                                                                             \
                                                                                  \
    void Register() override                                                      \
    {                                                                             \
        OnEffectApply  += AuraEffectApplyFn(CLASS::OnApply, EFFECT_0,              \
                                            SPELL_AURA_DUMMY,                     \
                                            AURA_EFFECT_HANDLE_REAL);             \
        OnEffectRemove += AuraEffectRemoveFn(CLASS::OnRemove, EFFECT_0,            \
                                             SPELL_AURA_DUMMY,                    \
                                             AURA_EFFECT_HANDLE_REAL);            \
    }                                                                             \
};

COLLISION_AURA_SCRIPT(spell_collision_block_enemies,  PlayerCollisionServer::RULE_BLOCK_ENEMIES)
COLLISION_AURA_SCRIPT(spell_collision_block_all,      PlayerCollisionServer::RULE_BLOCK_ALL)
COLLISION_AURA_SCRIPT(spell_collision_mutual_enemies, PlayerCollisionServer::RULE_COLLIDE_ENEMIES)
COLLISION_AURA_SCRIPT(spell_collision_mutual_all,     PlayerCollisionServer::RULE_COLLIDE_ALL)

#undef COLLISION_AURA_SCRIPT

void AddSC_custom_player_collision()
{
    RegisterSpellScript(spell_collision_block_enemies);
    RegisterSpellScript(spell_collision_block_all);
    RegisterSpellScript(spell_collision_mutual_enemies);
    RegisterSpellScript(spell_collision_mutual_all);
}
