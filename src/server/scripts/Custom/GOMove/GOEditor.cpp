/*
 * GameObject editor - server-side "click to preview, then commit" flow.
 *
 * This is the fully server-driven placement mode built on top of GOMove. It does
 * NOT need any client modification: you place an object by aiming the reticle
 * spell (27651) and clicking, which drops an unsaved GM-only "ghost" preview at
 * the target point. Reposition by clicking again, rotate with `.goeditor turn`,
 * then `.goeditor commit` to save a permanent object, or `.goeditor off` to bail.
 *
 * Compared to the planned client-side ghost (see EDITOR_DESIGN.md), the tradeoff
 * is: placement is click-to-preview (discrete) rather than hover-follow
 * (continuous), and the ghost is opaque rather than translucent. Everything else
 * - GM-only visibility, rotation, commit/cancel - works with pure core code.
 *
 * GM-only visibility uses an additive phase bit: `.goeditor on` adds
 * EDITOR_PHASE_BIT to the GM's phase (so they still see the normal world) and the
 * ghost is spawned into that bit only, so ordinary players (phase 1) never
 * receive it. The committed permanent object is spawned back into the GM's
 * original phase so everyone sees it.
 */

#include "GOMove.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "GameObject.h"
#include "Map.h"
#include "MapManager.h"
#include "Object.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Position.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "SpellScript.h"
#include <cmath>
#include <unordered_map>

using namespace Trinity::ChatCommands;

namespace
{
    // High phase bit reserved for editor ghosts. The GM temporarily gains this
    // bit on top of their real phase; ghosts carry only this bit, so ordinary
    // players (who lack it) never see them. One builder at a time is assumed.
    constexpr uint32 EDITOR_PHASE_BIT = 0x40000000;

    struct EditorSession
    {
        uint32 entry = 0;               // template being placed
        ObjectGuid ghostGuid;           // current ghost (empty = none live)
        uint32 savedPhase = PHASEMASK_NORMAL;
        float orientation = 0.0f;       // ghost facing, radians
    };

    // Keyed by player GUID. Touched only from the world update thread (command
    // handlers, spell AfterCast, logout), so no locking is required.
    std::unordered_map<ObjectGuid, EditorSession> g_sessions;

    EditorSession* FindSession(Player* player)
    {
        auto it = g_sessions.find(player->GetGUID());
        return it != g_sessions.end() ? &it->second : nullptr;
    }

    GameObject* FindGhost(Player* player, EditorSession const& s)
    {
        if (s.ghostGuid.IsEmpty())
            return nullptr;
        return player->GetMap()->GetGameObject(s.ghostGuid);
    }

    void DespawnGhost(Player* player, EditorSession& s)
    {
        if (GameObject* go = FindGhost(player, s))
            go->Delete();
        s.ghostGuid.Clear();
    }

    // Spawns a temporary (non-DB) ghost of s.entry at the given point, in the
    // editor phase, and records it on the session. Any previous ghost should be
    // despawned by the caller first.
    GameObject* SpawnGhost(Player* player, EditorSession& s, float x, float y, float z, float o)
    {
        GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(s.entry);
        if (!info)
            return nullptr;

        Map* map = player->GetMap();
        if (!MapManager::IsValidMapCoord(player->GetMapId(), x, y, z))
            return nullptr;

        GameObject* go = new GameObject();
        ObjectGuid::LowType lowGuid = map->GenerateLowGuid<HighGuid::GameObject>();
        QuaternionData rot = QuaternionData::fromEulerAnglesZYX(o, 0.0f, 0.0f);
        if (!go->Create(lowGuid, s.entry, map, EDITOR_PHASE_BIT, Position(x, y, z, o), rot, 255, GO_STATE_READY))
        {
            delete go;
            return nullptr;
        }

        go->SetRespawnTime(0);              // temporary - never persisted
        go->SetSpawnedByDefault(false);
        if (!map->AddToMap(go))             // AddToMap deletes go on failure
            return nullptr;

        s.ghostGuid = go->GetGUID();
        s.orientation = o;
        return go;
    }

    void RestorePhase(Player* player, EditorSession const& s)
    {
        if (player->GetPhaseMask() != s.savedPhase)
            player->SetPhaseMask(s.savedPhase, true);
    }
}

// Reticle placement: casting spell 27651 while in an editor session moves the
// ghost to the clicked point. When NOT in a session this is a no-op and the
// separate GOMove_spell_place script keeps its own spawn-from-queue behaviour.
class GOEditor_spell_place : public SpellScriptLoader
{
public:
    GOEditor_spell_place() : SpellScriptLoader("GOEditor_spell_place") { }

    class spell_impl : public SpellScript
    {
        PrepareSpellScript(spell_impl);

        void HandleAfterCast()
        {
            Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
            if (!player)
                return;
            EditorSession* s = FindSession(player);
            if (!s)
                return;                     // not editing
            WorldLocation const* dest = GetExplTargetDest();
            if (!dest)
                return;

            DespawnGhost(player, *s);
            if (!SpawnGhost(player, *s, dest->GetPositionX(), dest->GetPositionY(), dest->GetPositionZ(), s->orientation))
                ChatHandler(player->GetSession()).PSendSysMessage("Editor: could not place ghost here.");
        }

        void Register() override
        {
            AfterCast += SpellCastFn(spell_impl::HandleAfterCast);
        }
    };

    SpellScript* GetSpellScript() const override
    {
        return new spell_impl();
    }
};

class GOEditor_commandscript : public CommandScript
{
public:
    GOEditor_commandscript() : CommandScript("GOEditor_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable editorTable =
        {
            { "on",     HandleOnCommand,     rbac::RBAC_PERM_COMMAND_GOBJECT_ADD_TEMP, Console::No },
            { "commit", HandleCommitCommand, rbac::RBAC_PERM_COMMAND_GOBJECT_ADD_TEMP, Console::No },
            { "turn",   HandleTurnCommand,   rbac::RBAC_PERM_COMMAND_GOBJECT_ADD_TEMP, Console::No },
            { "cancel", HandleCancelCommand, rbac::RBAC_PERM_COMMAND_GOBJECT_ADD_TEMP, Console::No },
            { "off",    HandleCancelCommand, rbac::RBAC_PERM_COMMAND_GOBJECT_ADD_TEMP, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "goeditor", editorTable },
        };
        return commandTable;
    }

    static bool HandleOnCommand(ChatHandler* handler, uint32 entry)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!sObjectMgr->GetGameObjectTemplate(entry))
        {
            handler->PSendSysMessage("Editor: gameobject template %u does not exist.", entry);
            handler->SetSentErrorMessage(true);
            return false;
        }

        EditorSession& s = g_sessions[player->GetGUID()];
        // Re-entering with a new entry: drop the old ghost but keep the already
        // saved phase so we don't stack the editor bit onto itself.
        if (s.entry != 0)
            DespawnGhost(player, s);
        else
            s.savedPhase = player->GetPhaseMask();

        s.entry = entry;
        s.orientation = player->GetOrientation();
        player->SetPhaseMask(s.savedPhase | EDITOR_PHASE_BIT, true);

        DespawnGhost(player, s);
        SpawnGhost(player, s, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), s.orientation);

        handler->PSendSysMessage("Editor: placing gameobject %u. Aim the reticle spell (27651) and click to position it, "
                                 ".goeditor turn <deg> to rotate, .goeditor commit to save, .goeditor off to cancel.", entry);
        return true;
    }

    static bool HandleTurnCommand(ChatHandler* handler, float degrees)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        EditorSession* s = FindSession(player);
        if (!s || s->entry == 0)
        {
            handler->PSendSysMessage("Editor: no active session. Use .goeditor on <entry> first.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        s->orientation = Position::NormalizeOrientation(s->orientation + degrees * float(M_PI) / 180.0f);
        if (GameObject* go = FindGhost(player, *s))
        {
            float x = go->GetPositionX(), y = go->GetPositionY(), z = go->GetPositionZ();
            DespawnGhost(player, *s);
            SpawnGhost(player, *s, x, y, z, s->orientation);
        }
        handler->PSendSysMessage("Editor: ghost facing %.1f deg.", s->orientation * 180.0f / float(M_PI));
        return true;
    }

    static bool HandleCommitCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        EditorSession* s = FindSession(player);
        if (!s || s->entry == 0)
        {
            handler->PSendSysMessage("Editor: no active session. Use .goeditor on <entry> first.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        GameObject* ghost = FindGhost(player, *s);
        if (!ghost)
        {
            handler->PSendSysMessage("Editor: no ghost placed yet - aim the reticle and click first.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        float x = ghost->GetPositionX(), y = ghost->GetPositionY(), z = ghost->GetPositionZ(), o = ghost->GetOrientation();

        // Save a real, persistent object in the GM's ORIGINAL phase so everyone
        // sees it (SpawnGameObject also sends the addon ADD for the GOMove UI).
        GameObject* saved = GOMove::SpawnGameObject(player, x, y, z, o, s->savedPhase, s->entry);
        if (!saved)
        {
            handler->PSendSysMessage("Editor: failed to save the object here.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Drop the preview; keep the session open so the GM can place the next
        // one with another reticle click.
        DespawnGhost(player, *s);
        handler->PSendSysMessage("Editor: saved gameobject %u (guid %u). Click to place another, or .goeditor off.",
                                 s->entry, saved->GetSpawnId());
        return true;
    }

    static bool HandleCancelCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        EditorSession* s = FindSession(player);
        if (!s)
        {
            handler->PSendSysMessage("Editor: no active session.");
            return true;
        }

        DespawnGhost(player, *s);
        RestorePhase(player, *s);
        g_sessions.erase(player->GetGUID());
        handler->PSendSysMessage("Editor: session ended.");
        return true;
    }
};

class GOEditor_player : public PlayerScript
{
public:
    GOEditor_player() : PlayerScript("GOEditor_player") { }

    void OnLogout(Player* player) override
    {
        auto it = g_sessions.find(player->GetGUID());
        if (it == g_sessions.end())
            return;
        DespawnGhost(player, it->second);
        RestorePhase(player, it->second);
        g_sessions.erase(it);
    }
};

void AddSC_GOEditor()
{
    new GOEditor_spell_place();
    new GOEditor_commandscript();
    new GOEditor_player();
}
