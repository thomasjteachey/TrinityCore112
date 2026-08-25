/*
 * "Obstructing Presence" - the wearer eclipses hostile line of sight.
 *
 * While the aura is up, enemies cannot cast THROUGH the wearer at something
 * behind them. Spells aimed at the wearer itself still land, and allies are
 * unaffected.
 *
 * This is the server-side sibling of the client-side player collision. Collision
 * can live in the client because movement is client-authoritative; line of sight
 * is decided by the server, so it has to be enforced here or it would be
 * cosmetic and bypassed by simply not running the client tweak.
 *
 * The script does nothing but register and unregister the wearer - all of the
 * geometry lives in LosBlocker, and the spell LoS checks in Spell::CheckCast
 * consult it. Registration is what keeps the cost at zero when nobody has the
 * aura: LosBlocker::Active() is a single atomic load.
 *
 * Bound by spell_script_names, like every other custom spell script here.
 */

#include "ScriptMgr.h"
#include "SpellScript.h"
#include "SpellAuraEffects.h"
#include "LosBlocker.h"
#include "Unit.h"

class spell_los_blocker : public AuraScript
{
    PrepareAuraScript(spell_los_blocker);

    void OnApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* target = GetTarget())
            LosBlocker::Add(target, GetId());
    }

    void OnRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* target = GetTarget())
            LosBlocker::Remove(target);
    }

    void Register() override
    {
        OnEffectApply  += AuraEffectApplyFn(spell_los_blocker::OnApply,
                                            EFFECT_0, SPELL_AURA_DUMMY,
                                            AURA_EFFECT_HANDLE_REAL);
        OnEffectRemove += AuraEffectRemoveFn(spell_los_blocker::OnRemove,
                                             EFFECT_0, SPELL_AURA_DUMMY,
                                             AURA_EFFECT_HANDLE_REAL);
    }
};

void AddSC_custom_los_blocker()
{
    RegisterSpellScript(spell_los_blocker);
}
