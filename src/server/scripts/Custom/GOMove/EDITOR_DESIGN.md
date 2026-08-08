# In-client GameObject Editor (option 3) — design

Goal: a **transparent preview of a gameobject that follows the mouse cursor**, in
the live client, before you commit it to the world. Rotate with the mousewheel,
left-click to place. This is the "real editor ghost" — Epsilon-style — built on
top of the GOMove integration already on this branch.

Status: **design only, no code yet.** The client half is blocked on reverse-
engineering three addresses in the running `Wow.exe` (see "RE targets"), which
has to be done with a debugger on the client machine.

---

## The two constraints that shape everything

1. **The client never tells the server where the cursor points.** Ground-target
   aiming is computed client-side; the server only learns the point at cast time.
   So a ghost that *follows the hover* cannot be driven from the server — the
   cursor→world position has to be read inside the client, every frame.

2. **The 3.3.5 protocol has no gameobject transparency.** Our update fields expose
   only GO dynamic flags (activate/animate/sparkle — `GameObject.cpp` ~2811).
   There is no alpha the server can set. Translucency has to be applied
   client-side at render time.

Both point the same way: the ghost is a **client-side rendering trick**, and the
server's only jobs are to spawn a preview object and to save the real one on
commit.

---

## Chosen architecture: render-hijack hybrid

> The ghost is a **real GameObject** the server spawns once and never moves. The
> client **overrides that object's render transform and alpha every frame** — to
> the cursor raycast point, at ~40% opacity — for its GUID only.

Why this beats the alternatives we considered:

- **vs. drawing an arbitrary M2/WMO ourselves each frame:** that means driving the
  client's async model loader and inserting draw calls into the world render pass
  for a non-managed object — weeks of pipeline RE. Here the object is real, so the
  client loads and draws the model *for free*. We only hijack its transform.
- **vs. moving a server GO to follow the cursor:** hits the destroy/recreate
  flicker (the 3.3.5 client caches GO positions, which is the whole reason
  `GOMove::MoveGameObject` deletes and reloads) *and* floods the network every
  frame. The hijack never moves the server object, so neither problem exists.

The remaining unknowns are all *local reads/writes* in the client, not renderer
authoring: find the object by GUID, override its transform before draw, pin its
alpha. That's tractable with the hooking framework AnimSpeedFix already has.

### Where the client code lives

**Into AnimSpeedFix, not a new DLL.** Only one `dinput8.dll` proxy can sit next
to `Wow.exe`, which is why tracing was already folded into AnimSpeedFix. The
editor is a new module (`GOEditor.h/.cpp`) compiled into the same DLL, gated by a
`[GOEditor]` ini section (default **off** — this is a GM tool, not a player fix).
It reuses the existing `PlaceDetour` / naked-stub / `IsReadable` machinery
verbatim.

### GM-only visibility — no core change needed

The ghost must not appear (static and opaque) to normal players. Solution: spawn
it into a **dedicated phase** that only the editing GM occupies for the duration
of a session. GOs already carry `SetPhaseMask`. No per-observer packet surgery
(unlike the transmog system) — the phase does it. The `.goeditor on` command
stashes the GM's real phase, moves them to the editor phase; `.goeditor off`
restores it. Ghost spawns use that phase mask.

---

## Components

### Client (AnimSpeedFix / `GOEditor.cpp`)

- **Per-frame tick.** Hook the world-frame render (or the existing scene update)
  to run our logic once per frame. Candidate: reuse the frame cadence already
  reachable from the render pass; exact site is RE target #4 (optional — see note).
- **Cursor→world raycast.** Each frame, project the cursor into the world and
  raycast against terrain+WMO to get the hit point. This is the same call the
  ground-target reticle uses. RE target #1.
- **Find the ghost GO by GUID.** Look the ghost up in the client object manager
  by the low-GUID the server told us. RE target #2.
- **Override transform + alpha.** Write the ghost's render position/orientation to
  the raycast hit + current wheel rotation, and pin its alpha to ~0.4, right
  before it draws. RE target #3.
- **Input.** Mousewheel rotates the ghost (accumulate an angle). Left-click sends
  the commit. Escape / right-click cancels. Read via the client's input path or a
  low-level hook; the addon UI can also drive mode changes.

### Server (this repo, `gameobject_editor` branch)

Extends the GOMove command script. New `.goeditor` subcommands (RBAC 390, same
gate as `.gomove`, so still GM-only per the three-layer check from last session):

- `.goeditor on <entry>` — enter editor phase, spawn a **ghost** GO of `<entry>`
  in that phase near the player, unsaved, and send its low-GUID to the client
  addon (`GOMOVE`/`CCGAME` channel). Client begins the cursor-follow.
- `.goeditor commit <guid> <x> <y> <z> <o>` — client sends the final placement on
  click; server saves a real, persistent GO there via `GOMove::SpawnGameObject`
  and despawns the ghost.
- `.goeditor cancel <guid>` — despawn the ghost, restore phase.

The ghost itself is just a normal `GameObject::Create` + add-to-map with **no
`SaveToDB`** (temporary, like GOMove's hex-guid objects), in the editor phase.

### Protocol

Reuse the existing addon-message channel (`CCGAME\t<KEY>:<value>`, invisible in
chat — see client-patch-layout). New keys:

- server→client `GHOST:<lowguid>:<entry>` — a ghost is live, start following.
- server→client `GHOSTOFF:<lowguid>` — ghost gone, stop.
- client→server placement uses the existing `.goeditor commit ...` chat command
  (same path GOMove already uses for `.gomove`), so no new opcode is required.

A custom opcode through the `0x632013` dispatch table is the alternative if the
click→commit latency over the chat path proves visible, but start with the chat
command — it reuses everything and needs zero new client packet code.

---

## RE targets (must be found on the running client, build 12340)

Addresses below are **candidates / patterns to verify**, not confirmed. Verify
each with a byte-signature check the way `kGateSig` etc. are pinned in
AnimSpeedFix before writing a hook. Never hardcode an unverified site.

1. **Cursor→world raycast (`CWorld::Intersect` / TraceLine).**
   - What: screen ray vs. terrain+WMO, returns a world hit point.
   - Find it: it is what the green ground-target reticle uses. Break on the
     ground-target spell cast (e.g. Blizzard/Flare) and walk back to the function
     that produces the cast destination; or search for the `CGWorldFrame`
     projection helpers. Flags arg selects terrain/WMO/M2/liquid.
   - Verify: call it with the current cursor and confirm the returned point tracks
     the reticle exactly.

2. **Client object manager lookup by GUID (`ClntObjMgr::ObjectPtr`).**
   - What: resolve a low/base GUID to the client `CGGameObject_C*`.
   - Find it: well-documented singleton on 12340; the GUID→object getter is a
     small function taking (typeMask, guid). Cross-check against the object
     manager base already referenced near the packet path.
   - Verify: look up the player's own GUID and confirm you get their object.

3. **GO render transform + alpha application point.**
   - What: where a GO's world position becomes its draw matrix, and where its
     per-instance alpha is submitted, each frame.
   - Find it: the client fades new GOs in — so a per-GO alpha ramp already exists;
     trace the fade to the field/multiply it reads. For the transform: conditional
     breakpoint on a read of the ghost GO's position during render.
   - Two viable implementations, settle empirically:
     (a) override the computed matrix just before the draw call, or
     (b) poke the position field **and** invalidate the cached matrix so it
         recomputes. (a) is cleaner; (b) works if there's an easy dirty flag.

4. **Per-frame hook site (optional).**
   - We may not need a new one — the SwingGuard/anim hooks already give a per-frame
     foothold, and input polling can hang off the render pass. Prefer reusing an
     existing cadence over adding a fourth patch site.

---

## Milestones (staged so a usable tool ships before the hard polish)

1. **Server ghost lifecycle.** `.goeditor on/commit/cancel`, editor phase, ghost
   spawn/despawn, addon messages. Fully testable server-side with a static ghost
   (no client mod yet) — you'll see an opaque object appear/despawn.
2. **Cursor raycast read (RE #1).** Client logs the world hit point each frame.
   Pure read, no rendering change — de-risks the hardest unknown first.
3. **Transform override (RE #2 + #3a).** Ghost follows the cursor, **opaque**.
   This is already a huge usability win — hover-to-place — even before alpha.
4. **Alpha pin (RE #3 alpha).** Ghost renders translucent. If this proves too
   deep, the fallback is the "recreate-each-move mid-fade" translucency, or simply
   shipping the opaque ghost from milestone 3.
5. **Rotation + input polish.** Mousewheel rotate, click-commit, esc-cancel,
   snap-to-ground, multi-place.

Ordering deliberately puts the two hardest RE items (raycast, alpha) at steps 2
and 4 with a shippable result in between, so the project produces something usable
even if alpha stalls.

---

## Open questions

- Editor phase: a fixed reserved phase mask, or allocate per-GM? Fixed is simpler
  and fine for one builder at a time.
- Ghost model coverage: GOs display as either M2 or WMO; the hijack is model-
  agnostic because the object is real, so both should "just work" — confirm WMO
  transform override behaves at milestone 3.
- Click capture: low-level mouse hook vs. reading the client's own click state.
  Prefer the latter to avoid fighting the client for input focus.
