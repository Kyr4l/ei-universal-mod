# game.exe class hierarchy — working notes (phase 1; the finished page will be docs/game-exe/classes.md)

Conventions: `Class::vN` = slot N of the class's vtable not yet understood. Root base class names in quotes are guesses
(no class-name stub: the roots are abstract); the stub names (CUnit, CTree, ...) are the real ones. Offsets are `this+off`.

## "CObject" — the root of every game class (13 virtual slots; 150+ vtables)
Fields: +0 vtable, +4 refcount, +8 flags (bit 16 / bit 17 = per-class booleans, see slots 21 / 29 of CMapObject), +0x10 typeId
(the value slot 12 returns, stored at construction), +0x14 owning manager (CManagerBase*), +0x18 ?. Size 0x1c.

| slot | name | what it does (default) |
|---|---|---|
| 0 | ~dtor(flags) | deleting destructor: body, then `operator delete` if flags&1 |
| 1 | CopyBase(other) | copies +8, +0x10, +0x18 (0x402D40) |
| 2 | Init() -> bool | per class |
| 3 | Done() | per class |
| 4 | Release() | --refcount; at 0: manager(+0x14)->vt[24](this) (0x455980) |
| 5 | v5 | empty default (0x402CC0) |
| 6 | v6 | returns 0 (0x402CD0) |
| 7 | GetName() | "noname" (0x402CE0) |
| 8 | GetDesc() | "nodesc" (0x402CF0) |
| 9 | Read(stream) | default empty; stream: vt[1](ptr,size) read, +8 version |
| 10 | Write(stream) | default empty; stream: vt[2](ptr,size) write |
| 11 | ClassName() | the class-name stub |
| 12 | GetTypeId() | small constant (CCamera 0x43, CTree 0x52, CSpell 0x4f, CMapObjectServer? 0x47 = the "specific" type CMapObject expects, 0x39 = a figure-holding specific) |

## "CObject3D" — CObject + transform (slots 13..17 shared by CCamera, CParticle, CLight, the CEffect*, CTree.., CUnit..)
Fields: +0x1c local pos (3f), +0x28 world pos (3f), +0x34 local rot quat (4f), +0x44 world rot quat (4f) -> size 0x54.
| 13 | CopyTransform(other) | 0x401490 |
| 14 | UpdateWorldRot(parentQuat) | 0x40CB80 |
| 15 | UpdateWorldTransform(parent) | 0x40CC30: slot 14 then world pos |
| 16 | WriteTransform(buf) | buf->vt[14] (0x401510) |
| 17 | ReadTransform(buf) | buf->vt[13] (0x401540) |

## "CMapObject" — the placed world objects (CTree, CBuilding, CStone, CStuffObject, CTrapObject, CMagicTrapObject(43 slots), CLeverObject, CTorchObject: 61 slots; CUnit 62; CFakeUnit 63)
Fields so far: +0xa4 name (char*), +0xc4 offset (3f, reset from +0x1ac), +0xd0 array (ptr, count +0xd4, cap +0xd8, step +0xdc;
32-byte aligned), +0x100 RECT, +0x128 offset (3f, slot 48), +0x170 "specific" (CMapObjectServer / ClientSpecific, typeId 0x47:
its +0x1c = owner back pointer, +4 refcount), +0x174 = 5, +0x178 = 1.0f, +0x17c struct (0x5233B0 init / 0x522C50 copy),
+0x194 (0x50FE70 init), +0x198/+0x19c array of 0x14-byte items (0x688550 read, 0x6885A0 write), +0x1a8 string, +0x1ac 3f, +0x1c0 = -1, +0x1c4 (0x50FA30/0x47FB70).
Slots 18..61: see functions.tsv (CMapObject::*). Known: 19 SetSpecific, 20 Assign, 21/29 flag bits, 30 ResetOffset, 48 PlaceOnTerrain,
51 SnapZ, 52/53 GetLocalPos, 55 -> specific vt[17]. The world object: 0x455950(this) -> its vt[27](x,y) = terrain height.

## The root fields, settled
+4 refcount, +8 flags, +0xc id (set by the manager, -1 before), +0x10 **kind** = the creation key passed to
CBaseManager::RegisterInstant(kind, id) (NOT GetTypeId: the server and client factories map the same kind to different
classes), +0x14 manager, +0x18 (read from the save stream by CBaseManager::Read). Objects attached to another one keep the
owner at +0x1c (the "specific" objects: CSpecificBase, vtable 0x73DC54).

## Kinds (the factory keys; obj+0x10)
| kind | CServerSpecificManager (0x4D6970) | CClientSpecificManager (0x42D700) | CMapObjectManager (0x512730) |
|---|---|---|---|
| 0x32-0x34 | | | CUnit (0x4dc bytes) |
| 0x35 | | | CTree (0x238) |
| 0x36 / 0x37 / 0x38 | | | ctors 0x5297C0 / 0x5298A0 / 0x529980 (0x238 each: CBuilding / CStone / CStuffObject, see ctor.py) |
| 0x39 | | | CFakeUnit (0x580) |
| 0x3a / 0x3b / 0x3c | | | ctors 0x52BEE0 (0x23c) / 0x529DD0 (0x290) / 0x52B1F0 (0x264) |
| 0x300d / 0x3010 | | | ctors 0x50C5F0 (0x78) / 0x529A80 (0x23c) |
| 0x45 | | CFigureClientSpecific (0x3c) | |
| 0x46 | CObject3DServer (0x20) | CObject3DClientSpecific (0xfc) | -> attached at CMapObject +0x94 |
| 0x47 | CMapObjectServer (0x94) | CMapObjectClientSpecific (0x58) | -> attached at CMapObject +0x170 ("specific") |
| 0x48 | CUnitServer (0xb20) | CUnitClientSpecific (0xdc) | -> attached at CUnit +0x240 |
| 0x49 | CPlayerServer (0x5128) | CPlayerClient (0x50f8) | |
| 0x4b | CTorchObjectSpecific (0x58) | CTorchObjectClientSpecific (0x64) | |

## Managers — "CBaseManager" (vtable 0x73B870, 25 slots; the name comes from the string "CBaseManager::RegisterInstant")
Fields: +0x1c id->object map, +0x30 max id, +0x38/+0x3c list head/tail (+0x40 count, +0x44 free nodes, +0x48 pool),
+0x50 id->node hash, +0x68 kind->pooled objects, +0x80 kind->pool count, +0x98/+0x9c id range (0, 2000000000), +0xa8 last id,
+0xac the world (set by slot 13 SetWorld). Slots: 2 Init, 3 Done, 9 Read, 10 Write, 13 SetWorld(world), 14 CreateByKind(kind)
(pure virtual in the base), 15 DoneObjects, 23 RegisterInstant(kind, id or -1 = random), 24 DestroyObject(obj) (from
CObject::Release). 16-22: defaults returning 0 / empty (per-manager hooks).

## CUnitServer (kind 0x48 server; vtable 0x73EF38, 54 slots; 0xb20 bytes) — +0x1c = the owning CUnit
+0x3c move state (slot 16 returns it; +0 mode, 2 = moving), +0x1e0 stats block (0x523xxx / 0x524xxx / 0x525xxx helpers,
+0x1e4 experience -> slot 24 GetLevel), +0x8e4 attack (short), +0x980 (0x5D5080), +0xa24 (0x5CEED0), +0xaf8 (0x5CCBF0).
CUnit fields seen from here: +0x24c player (0 = monster), +0x26c equipped weapon id, +0x330 struct (net section 4),
+0x340, +0x480..+0x488 / +0x48c..+0x498 (slots 37 / 36 check them), +0x1dc net dirty mask (MarkNetDirty), +0x1c0 last change time.

## CWorld (vtable 0x73F18C, 42 slots) -> CWorldClient (0x73F41C, 43) / CWorldServer (0x73F928, 42); CParty (0x743000) implements the same container slots 13..26
Managers (CWorld::CreateManagers 0x55EE40): +0x38 CPlayerManager, +0x3c CFigureManager, +0x40 CMapObjectManager, +0x44 the specific manager (CClientSpecificManager / CServerSpecificManager; NOT the terrain),
+0x48 CEffectManager, +0x4c CSpellManager, +0x50 CSpellConstructor? (0x683FF0), +0x54 CItemManager, +0x58 CItemConstructor? (0x6A5AB0),
+0x5c handle (vtable 0x73F240). +0x1c time base, +0x20 time scale/15, +0x64 world time (CMapObject reads world+0x68 as "now"),
+0x170 passability map (server; 0x5BA830 / 0x5BA9D0 / 0x5BBB20), +0x180 terrain object (client), +0x280c height map (server),
+0x12c64 aggro table, +0x12d74 id hash (server FindObjectById), +0x12e44 timed item list (server).
Slots: 13 GetWorld(self), 14 GetEffectManager, 15 GetFigureManager, 16 GetItemConstructor?, 17 GetSpellConstructor?, 18 GetItemManager,
19 GetMapObjectManager, 20 GetPlayerManager, 21 GetSpecificManager, 22 GetSpellManager, 23 Get34, 24 CreateItem(desc), 25 Get224 / 0,
26 IsServer, 27 GetHeight (server) / ClampToTerrain (client), 28 Passability (server) / GetHeight (client), 29/30 wrappers,
31 SetTime, 32 SetTimeScale, 33..38 client getters +0x188..+0x19c, 40 FindObjectById, 41/42 spatial queries (client).

## The "specific" objects (CSpecificBase 0x73DC54, 20 slots): +0x1c = owner object, created by kind through the server / client specific manager
- CObject3DServer (0x20 bytes) / CObject3DClientSpecific (0xfc): kind 0x46, at CMapObject +0x94. Client = the renderable: parts lists +0x20/+0x24/+0x28,
  scale +0xf0..+0xf8 (slot 18), slots 13..15 attach/detach parts, 19 (435 lines), 20 Render, 21 RebuildMaterials, 22..24 hit tests, FirstPart/NextPart iterators.
- CMapObjectServer (0x94) / CMapObjectClientSpecific (0x58): kind 0x47, at CMapObject +0x170. Server +0x20 = placement (floats: +8 HP, +0xc max HP,
  ...; packed to int16 at CMapObject +0x17c: +2 HP, +4 max HP); slots 17 IsDead, 18 IsDestroyed, 20 TakeHit (server).
- CUnitServer (0xb20) / CUnitClientSpecific (0xdc): kind 0x48, at CUnit +0x240 (see above); CUnitSpecific base 0x73EDBC.
- CPlayerServer (0x5128) / CPlayerClient (0x50f8): kind 0x49; +0x48 (server) = the connection (vt[2] = write bytes: messages 0x17, 0x0a, 0x14, 1);
  +0x20 state (1 in game, 3 leaving). CPlayer (13 slots, typeId 0xf) is the owner; CPlayerClient slot 14 OnUnitAdded.

## CFigure (vtable 0x73C1C4, 24 slots; typeId 0x44; three vtables share the stub: 0x73C1C4 full, 0x73C224 (20), 0x73FBF0 (22)); CI3DFigure (0x73FC48, 24, typeId 0x46) shares 15..23
+0x120 name, +0x158/+0x15c header counts, +0x1c0[]/+0x1d0[] rotation tables (0x40-byte entries). Slots: 7 GetName, 15 UpdateWorldTransform,
16 collect parts, 17 read parts, 18 CopyFrom, 19 Load(data) ("FIG" magic), 20 ParseFIG, 21 BuildMorphed(data, s, d, h), 22/23 negate rotations.

## Items — "CItem" base (vtable 0x743DA4, 22 slots; CItemWeapon 0x5c bytes, CItemArmor 0x8c, CItemQuick, CItemQuest, CItemLoot, CItemSpell; CInventory shares the base)
+0x18 proto ids (lo = material index into g_0x7C86D8 (0x50-byte entries), hi = weapon index into g_0x7C86F8 (0xb4-byte entries)),
+0x1c refcounted holder, +0x20/+0x24 count, +0x28/+0x2c durability cur/max, +0x30 regen, +0x34, +0x38 weight, +0x3c, +0x40, +0x44 object,
+0x48; weapon +0x4c..+0x58 ranges. Slots: 6 Update (regen), 9/10 Read/Write (save), 13 InitFromProto, 14 Equals, 15 FillInfo,
16 GetWeight, 17..20 text getters (name, description, ...), 21 Zero3. Created by CItemManager::CreateByKind (kinds 0x3004.. ).

## Effects — CObject3D + slots 18..23 for the 24-slot ones (CEffectSound, SoundSeq, AmbientSound, ArrowFigure, ShowVisibility, Moshka, Tornado):
18 Reset, 19 GetParam (float), 20 GetPos (this+0x1c), 21 ReadNet(stream, reader) (DeltaReader), 22 WriteNet(writer), 23 Play(flag).
CEffectPointLight / Particle / Wall / Lightning / Arrow have only the 18 CObject3D slots.

## Managers: CBaseManager (25) -> CObjectManager (proto list +0xb0..+0xb4, slots 16..22: FindProtoByName, .., ClearProtos; CMapObjectManager, CItemManager 26 slots)
Every manager's slot 14 = CreateByKind (the factory), slot 25 (26-slot managers) = a name getter (see functions.tsv).

## The UI framework (unnamed vtables 0x740000..0x742FFF; no class-name stubs: ClassName returns "Sorry, no other way!", typeId 0x45)
CObject3D-derived widgets with a secondary 13-slot event interface at this+0x94 (IUIEvents, vtable 0x7401D0 base; each class has
its own copy: slots 1..6 / 9..12 empty defaults, 7 / 8 forward to its slots 2 / 5). Primary vtables:
- CUIWidget (0x740F60, 25 slots; ctor 0x5FE700 41 callers): +0x98 parent, +0x34..+0x40 colour, +0xec/+0xf0 materials (kind 0x16),
  +0x108[] figures (+0x10c), +0x118[] element callbacks (+0x11c), +0x128[] buttons (+0x12c), +0x138[] sprites/texts (+0x13c).
  Slots 19 SetVisible, 20 Draw(renderer), 21 UpdateHover, 22 UpdateElements, 23 UpdateHoverAndButtons.
- CUIWindow (0x741008, 42 slots; ctor 0x600B20 31 callers; base CUIWindowBase? 0x73F7F8): + children windows +0x148[] (+0x14c);
  slots 19..24 recurse into the children, 25/26 pure virtual, 27..41 empty event handlers.
- 55-slot screens (0x74091C, 0x740A2C, 0x740D18, 0x74214C) and 26/27-slot widgets: the actual menus (strings: MainMenu, campinfo,
  saveload, globalmap, battle00, infoitem, option_difficulty, quit_msg, lmp_status_*) — read per module in phase 3.

## Other hierarchies (not CObject-derived; phase 3 per module)
- Net: NetConnection (0x73BF5C, 15 slots; seed names from net/README.md), NetServer 0x73BB70/0x73BBB4 (17), NetClient 0x73BDEC/0x73BE30 (17).
- Renderer: I3DFigure? (0x73D8A0, 25 slots, abstract) / C3DFigureImpl? (0x73D93C, 26: Load 2566 lines, Render 3211/2524; CI3DFigure::SetTexture at 11).
- Resource readers: IResReader? (0x73C8A4, 13, abstract) and the 13/14-slot classes at 0x73C8F8..0x73CA10 (ctors 0x4782E0..0x47CD40).
- Small 13-slot helper classes 0x73D128..0x73D294 (ctors 0x4BA1C0..0x4BA950), 0x73F0C4/0x73F0F8 (0x551AE0), 0x73FF40/0x73FF8C (AI/orders 0x5D0F60..).

## UnitStats block (CUnitServer +0x1e0, 0x704 bytes; UnitStats_* in functions.tsv; module 0x520000-0x529000)
+4 HP (float, with +0x700 fractional accumulator), +8 ?, +0xc/+0x14/+0x1c base strength/dexterity/intelligence, +0x10/+0x18/+0x20
current ones (base 25 = "0 skill"), +0x24/+0x28 ?, +0x2c/+0x30 move speed, +0x44, +0x50, +0x54 carry weight, +0x58/+0x5c crippled-limb
ratios, +0x60 flag, +0xb0../+0xe0.. 7-value pairs (base / current), +0x160.. 6 modifier groups (from the armor: AddArmorModifiers),
+0x110 + part*0xf4 = the 6 BODY PARTS (hit locations, "HL"): {+0 kind, +4 damage-type mask, 3 armor tiers x 7 damage types (stride 0x12 dwords),
+0xe0 state (0 none, >= 2 alive, 3 normal), +0xe4 HP, +0xe8 max HP, +0xec rate} (ApplyDamage, PickHitLocation, ApplyBodyPartPenalties),
+0x2f0 scale, +0x6c8..+0x6ca attribute skills (3 bytes), +0x6cb/+0x6cc weapon skills, +0x6ce..+0x6d0 magic schools, +0x6d3[9*4] perk
levels per category (perk table g_0x7AF274, 0x44-byte entries, +0x34 = bonus %), +0x6e3..+0x6ec perk indices used by the formulas, +0x6f8 flags,
+0x6fc stored max HP (monsters). The packed 0x108-byte form (UnitStats_PackToRecord) lives at CUnit +0x330 and goes on the net (section 4).
Placement (CMapObjectServer +0x20): +4 tick, +8 HP, +0xc max HP, +0x10 HP regen, +0x14 mana, +0x18 max mana, +0x1c mana regen, +0x24.. 7 pairs.
Tables: skills g_0x7AF268 (0x18-byte entries, +0xc name: first letter d/i/s = governing attribute), perks g_0x7AF274, weapons g_0x7C86F8
(0xb4-byte entries), materials g_0x7C86D8 (0x50), items g_0x7C8718 (200). 7 damage types: impaling, slashing, crushing, termical, chemical, electrical, general.

## CUnitServer orders (module 0x545000-0x557000; the bot / um-engine need these)
Order struct (0x24 bytes): +0 kind, +4 param (item id, hit location, skill, 0x18a95 = loot a corpse), +8 target object (refcounted), +0xc..+0x14 position,
+0x18 byte (exact point), +0x1c extra, +0x20 run flag. Current order at CUnitServer +0x190, queued order at +0x1b4 (DispatchQueuedOrder moves it).
Kinds: 0 wait, 1 move to pos (QueueOrderMoveTo), 2 direct move, 3 attack target (QueueOrderAttack; param = hit location 0..5 / 6 any),
4 cast at pos (QueueCastAt), 5 cast on target (QueueCastOn), 6 interact (param 0 use / 1 interact / 2 interact-other / 0x18a95 loot a corpse; QueueOrderInteract*/Talk),
7 / 8 script animation / cast animation (QueueScriptOrder), 9 none, 10 use consumable (QueueOrderKindA), 0xb sit / stance 4 (QueueOrderKindB).
States (+0x164): 0 walking, 1 moving (direct), 2 waiting, 3 attacking (anim), 4 casting (anim), 5 strike in progress (TickAttackAnim), 6 idle / arrived.
Stances: +0x168 current (owner +0x264: 0 stand?, 1, 2 sneak (3 = sneak flag), 4 sit), attack mode +0x170 (owner +0x268: 1 / 2, 3 = flag). Net section 6 = +0x264/+0x268/+0x26c.
Per tick: CUnitServer::Update -> Think (DispatchQueuedOrder, UpdateStanceFlags, ApplyPendingStance, per-kind handlers: ApproachTarget / ExecuteCombatOrder / TickCombatOrder)
-> TickState / TickAnimation / TickAttackAnim -> CompleteOrder (FireAttack / CastOrderSpell / ExecuteInteraction) -> TakeHit on the target.
Animation queues: 12-byte entries {anim byte, sub byte, flags byte, +4 duration (-1 infinite), +8 loop flag}; keys for 0x5301B0(owner, key, 1): attack kind mask | move mode flag (0x30000 / 0x20000 / 0x10000 / 0x8000) | action bits.

## AI logic classes (CUnitServer +0x980 "brain", 2026-10-17)

The brain struct at CUnitServer +0x980: +0 owner CUnitServer, +4 order kind (0 none, 4 cast at, 6 interact), +0x28 spell / param,
+0x2c target (refcounted), +0x30 target 2, +0x4c idle speed factor (random 0.8..1.0), +0xa4 ..., +0xd0 AiMemory (= CUnitServer +0xa50),
+0xe0 / +0xe4 current point, +0xe8 stay time. The logic objects are 13-slot classes: 0 dtor, 1 CopyBase (no-op), 2 Reset, 3 Clone,
4 Enter(brain) -> ticks, 5 Update(brain), 6 Leave(brain), 7 (ret 4), 8 GetKind, 9 IsStationary (bool), 10, 11 Read(stream), 12 Write(stream).
Kinds (slot 8; the game calls them motivations, see Brain::Tick 0x5D53B0): 0 Guard 0x73D128, 1 Patrol 0x73FF40, 2 Sentry 0x73D190,
3 Suspection 0x73D1F8, 4 Aggression 0x73D260, 5 CorpseWatcher 0x73D294, 6 Player 0x73F0F8, 7 Fear 0x73FF8C, 8 Tamed 0x73F0C4, 10 Follow 0x73D1C4. Abstract bases: 0x73D15C (stationary: kinds 0, 2, 10; Enter returns 50) and 0x73D22C (kinds 3..8).
mob "Model" codes (0 point, 1 guard, 2 patrol, 3 sentry, 4 player, 5 alert guard) -> Brain::BuildFromLogicSlot 0x5D6630: 1 Guard, 2 Patrol, 3 Sentry,
4 Player, 5 Guard at the alarm point; AggrMode 0 -> Suspection + Aggression + CorpseWatcher + Fear, 1 -> revenge Aggression, 2 -> CorpseWatcher + Fear,
3 -> CorpseWatcher + Fear(player). Each tick every motivation's Enter (slot 4) returns a priority (current + 100); the best becomes current
(+0x8c) and its Update (slot 5) runs. Brain layout: +0 CUnitServer, +4 order kind (0 idle, 1 moving, 2 move + actions, 3 attack, 4 cast, 5 interact,
6 follow), +8 move pos, +0x14 action CArray, +0x28 spell / action timer, +0x2c target, +0x30 target 2, +0x34 alarm flag, +0x35 AlarmNumb, +0x36 slot index,
+0x37 Active, +0x38 CallForHelp radius, +0x40 Wait, +0x44 / +0x48 ranges, +0x4c decay, +0x50 / +0x5c waypoints, +0x68 retry countdown,
+0x6c follow tick, +0x70 logic ObjRefList (+0x74 head), +0x8c current logic, +0x90 mob logic slots CArray (+0x94 data, +0x98 count, 0x4c each),
+0xa4 AiSpellInfos (two tables +0xa8 / +0xbc, max level +0xcc), +0xd0 AiMemory, +0xe0 point / +0xe8 stay (= +0x38.. of the brain? see AiLogicSuspection::Enter),
+0x114 player units, +0x148 / +0x154 tracked lists, +0x164 ignore pairs, +0x18c 'enemy known' flag.

## CAIMap (g_0x7B2798) and AiMemory (CUnitServer +0xa50) (2026-10-17)

CAIMap (class name from the "StimulusMap::Load" string; one global at 0x7B2798, size 0x1003c): +0 world, +4 current tick, +8..+0x10 eye position
of the unit being processed, +0x14..+0x1c view direction, +0x20 / +0x24 cell, +0x28 directional flag, +0x2c [1024 ticks][16 channels] stimulus
time buckets, +0x1002c cell buffer (0x40 bytes = 16 channel list heads per 8 x 8 m cell), +0x10030 row table, +0x10034 w, +0x10038 h.
Stimuli (footsteps, sounds; 0x30-byte nodes, free list g_0x7C53F0, pool g_0x7C43A8): +0 / +4 pos, +8 / +0xc pos copy, +0x10 tick,
+0x14 sight factor, +0x18 smell factor, +0x1c owner unit, +0x20 channel, +0x24 / +0x28 cell list links, +0x2c bucket link.
aiinfo.res: "normal.dat" -> g_0x7C43C4 (0x400 dwords, perception table), the [Logic] / balance keys -> AiInfo g_0x7C53F8 (see AiInfo_Load 0x5CE0F0).
AiMemory (0xb8 bytes at CUnitServer +0xa50 = brain +0xd0): +0x10 / +0x14 noticed position, +0x18 attention (0..1000, decays by +0x1c),
+0x1c notice kind (-1 footstep, -2 unit lost, -3 corpse / sound), +0x24 ObjRefList forced-seen, +0x40 seen units, +0x5c seen corpses,
+0x78 hostile corpses, +0x94 ObjTimePairs ignore list {unit, ttl}, +0xa8 Records18 delayed sounds {pos, delay, alarm flag, text}.
CUnitServer perception fields: +0x244 / +0x248 stimulus channel masks, +0x294 / +0x2a4 / +0x2ac / +0x2b4 / +0x2bc / +0x2c4 / +0x2d4 /
+0x2dc / +0x2e4 sense factors (sight, sense life, hearing, smell), +0x224 view cone cos, +0x228 peripheral range, +0x8fc / +0x900 /
+0x904 range multipliers, +0xa68 alertness level, +0xaf8 Records18 heard sounds, +0xb0c flag, +0xb10 alertness object (0x5AEA60).

## CAIMap (g_0x7B00D0, the AI / navigation world; class name from "CAIMap::Load" strings) (2026-10-17)

+0 batching flag, +1 dirty, +2 nav grid updates enabled, +4..+0x10 dirty rect, +0x14 / +0x54 / +0x94 step cost tables (16 x 0x400 = 1.0),
+0xd4 / +0xe8 slope cost vectors (0x3ff entries, centred), +0xfc material table (0x118-byte entries: +0x104 speed, +0x114 packed cost nibbles),
+0x10c bridge hash (saved cell heights), +0x120 height scale (511 / max height), +0x124 CollisionMap (+4 w, +8 h sectors of 32 x 32 cells, +0x14 sectors
of 0x54 bytes: +4 object ObjRefList, +0x20 unit ObjRefList, +0x3c / +0x40 object volume entries {zmin, zmax, type, flag, next}), +0x14c / +0x1a0 cell
object record arrays (0x18 bytes: x, y, zmin, zmax, type, flag), +0x1f4 obstacle byte grid (+0x1f8 rows), +0x204..+0x278 step tables, +0x27c terrain
StampMap and +0x12a4 obstacle StampMap (1024 buckets of 4 x 4 byte stamps, 0x24-byte nodes), +0x22f8 NavGrid (two PathSearch instances, 8 class layers
of w / 8 x h / 8 nav cells: +0x120 neighbour costs (shorts), +0x1a0 best sub-cell nibbles, +0x220 region ids; local PathFinder +0x98), +0x2594 / +0x2614
local PathFinders (34 x 34 windows of 0x14-byte nodes), +0x2698 terrain grid (+0x269c rows of 9-byte cells: dword passability nibbles per class, ushort
+4 height, ushort +6 water depth (6 bits) / bit 6 bridge / bit 7 water, byte +8 free radius), +0x26a0 w, +0x26a4 h, +0x26a8 flags grid (+0x26ac rows:
bits 0..4 material, 0x80 sector chain, 0x100 empty, bits 9..11 inline object type, 12..21 zmax, 22.. zmin), +0x26b8 occupancy byte grid (+0x26bc rows,
direction codes 1..8 while path finding), +0x26c8 StimulusMap. Cells are 0.5 m (world pos * 2 - 0.5); nav cells are 8 x 8 cells; sectors 32 x 32.
Entry points: CAIMap::Load 0x5B43C0, CAIMap::FindPath 0x5BB350, CAIMap::StampObject 0x5B6A80, CAIMap::UpdatePassability 0x5B8FA0,
CAIMap::TraceVisibility 0x5BC3E0, CollisionMap::FindUnitsNear 0x5C0BD0.
